rhi::TextureHandle TryGetTextureHandle_(ResourceManager& rm, std::string_view id) const noexcept
{
	if (auto texRes = rm.Get<TextureResource>(id))
	{
		const auto& gpu = texRes->GetResource();
		if (gpu.id != 0)
		{
			return rhi::TextureHandle{ static_cast<std::uint32_t>(gpu.id) };
		}
	}
	return {};
}

rhi::TextureDescIndex GetOrCreateTextureDesc_(
	ResourceManager& rm,
	BindlessTable& bindless,
	std::string_view textureId)
{
	const std::string key{ textureId };

	if (auto it = textureDesc_.find(key); it != textureDesc_.end())
	{
		return it->second;
	}

	const rhi::TextureHandle handle = TryGetTextureHandle_(rm, textureId);
	if (!handle)
	{
		return 0;
	}

	const rhi::TextureDescIndex index = bindless.RegisterTexture(handle);
	textureDesc_.emplace(key, index);
	return index;
}

MaterialHandle GetMaterialHandle_(std::string_view materialId) const noexcept
{
	if (materialId.empty())
	{
		return {};
	}

	auto it = materialHandles_.find(std::string(materialId));
	if (it == materialHandles_.end())
	{
		return {};
	}
	return it->second;
}

MeshHandle GetOrLoadMeshHandle_(const LevelAsset& asset, AssetManager& assets, const std::string& meshId) const
{
	auto it = asset.meshes.find(meshId);
	if (it == asset.meshes.end())
	{
		throw std::runtime_error("Level: node references unknown meshId: " + meshId);
	}

	MeshProperties p{};
	p.filePath = it->second.path;
	p.debugName = it->second.debugName;
	p.flipUVs = it->second.flipUVs;
	p.submeshIndex = it->second.submeshIndex;
	p.bakeNodeTransforms = it->second.bakeNodeTransforms;
	return assets.LoadMeshAsync(meshId, std::move(p));
}

const LevelModelDef& GetModelDef_(const LevelAsset& asset, const std::string& modelId) const
{
	auto it = asset.models.find(modelId);
	if (it == asset.models.end())
	{
		throw std::runtime_error("Level: node references unknown modelId: " + modelId);
	}
	return it->second;
}

std::vector<int> MakeDrawsForModelNode_(const LevelAsset& asset, Scene& scene, AssetManager& assets, int nodeIndex, const LevelNode& node)
{
	const LevelModelDef& md = GetModelDef_(asset, node.model);
	const ImportedModelScene meta = LoadAssimpScene(md.path, md.flipUVs);
	std::vector<int> draws;
	draws.reserve(meta.submeshes.size());
	for (const ImportedSubmeshInfo& sub : meta.submeshes)
	{
		MeshProperties p{};
		p.filePath = md.path;
		p.debugName = md.debugName.empty() ? sub.name : (md.debugName + "_" + sub.name);
		p.flipUVs = md.flipUVs;
		p.submeshIndex = sub.submeshIndex;
		const std::string meshKey = node.model + "#submesh=" + std::to_string(sub.submeshIndex);
		MeshHandle mesh = assets.LoadMeshAsync(meshKey, std::move(p));

		std::string materialId = node.material;
		if (auto itOverride = node.materialOverrides.find(sub.submeshIndex); itOverride != node.materialOverrides.end())
		{
			materialId = itOverride->second;
		}

		DrawItem item{};
		item.mesh = mesh;
		item.material = GetMaterialHandle_(materialId);
		item.transform.useMatrix = true;
		item.transform.matrix = world_[static_cast<std::size_t>(nodeIndex)];
		const int drawIndex = static_cast<int>(scene.drawItems.size());
		scene.AddDraw(item);
		if (drawToNode_.size() < scene.drawItems.size())
		{
			drawToNode_.resize(scene.drawItems.size(), -1);
		}
		drawToNode_[static_cast<std::size_t>(drawIndex)] = nodeIndex;
		draws.push_back(drawIndex);
	}
	return draws;
}

EntityHandle GetEntityForNode_(int nodeIndex) const noexcept
{
	if (nodeIndex < 0)
	{
		return kNullEntity;
	}

	const std::size_t i = static_cast<std::size_t>(nodeIndex);
	if (i >= nodeToEntity_.size())
	{
		return kNullEntity;
	}

	const EntityHandle e = nodeToEntity_[i];
	if (e == kNullEntity)
	{
		return kNullEntity;
	}

	if (!ecs_.IsEntityValid(e))
	{
		return kNullEntity;
	}

	return e;
}

EntityHandle EnsureEntityForNode_(const LevelAsset& asset, int nodeIndex)
{
	if (nodeIndex < 0)
	{
		return kNullEntity;
	}

	const std::size_t i = static_cast<std::size_t>(nodeIndex);
	if (i >= asset.nodes.size())
	{
		return kNullEntity;
	}

	const LevelNode& node = asset.nodes[i];
	if (!node.alive)
	{
		return kNullEntity;
	}

	if (nodeToEntity_.size() < asset.nodes.size())
	{
		nodeToEntity_.resize(asset.nodes.size(), kNullEntity);
	}

	EntityHandle e = nodeToEntity_[i];
	if (e == kNullEntity || !ecs_.IsEntityValid(e))
	{
		e = ecs_.CreateEntity();
		nodeToEntity_[i] = e;
	}

	if (world_.size() < asset.nodes.size())
	{
		world_.resize(asset.nodes.size(), mathUtils::Mat4(1.0f));
	}

	ecs_.UpsertNodeData(e, nodeIndex, node.parent, node.transform, world_[i], Flags{ .alive = node.alive, .visible = node.visible });
	return e;
}

void DestroyEntityForNode_(int nodeIndex)
{
	if (nodeIndex < 0)
	{
		return;
	}

	const std::size_t i = static_cast<std::size_t>(nodeIndex);
	if (i >= nodeToEntity_.size())
	{
		return;
	}

	const EntityHandle e = nodeToEntity_[i];
	if (e != kNullEntity)
	{
		ecs_.DestroyEntity(e);
	}
	nodeToEntity_[i] = kNullEntity;
}

void SyncEntityRenderableForNode_(const LevelAsset& asset, Scene& scene, int nodeIndex)
{
	const EntityHandle e = GetEntityForNode_(nodeIndex);
	if (e == kNullEntity)
	{
		return;
	}

	const int drawIndex = GetNodeDrawIndex(nodeIndex);
	if (drawIndex < 0 || static_cast<std::size_t>(drawIndex) >= scene.drawItems.size())
	{
		if (ecs_.HasRenderable(e))
		{
			ecs_.RemoveRenderable(e);
		}
		return;
	}

	const DrawItem& di = scene.drawItems[static_cast<std::size_t>(drawIndex)];
	ecs_.UpsertRenderable(e, Renderable{ .mesh = di.mesh, .material = di.material, .drawIndex = drawIndex });

	if (nodeIndex >= 0)
	{
		const std::size_t i = static_cast<std::size_t>(nodeIndex);
		if (i < asset.nodes.size())
		{
			const LevelNode& node = asset.nodes[i];
			ecs_.UpsertNodeData(e, nodeIndex, node.parent, node.transform, world_[i], Flags{ .alive = node.alive, .visible = node.visible });
		}
	}
}

void EnsureDrawForNode_(const LevelAsset& asset, Scene& scene, AssetManager& assets, int nodeIndex)
{
	if (nodeIndex < 0)
	{
		return;
	}

	const std::size_t i = static_cast<std::size_t>(nodeIndex);
	if (i >= asset.nodes.size())
	{
		return;
	}

	const LevelNode& node = asset.nodes[i];
	if (!node.alive || !node.visible || (node.mesh.empty() && node.model.empty()))
	{
		DestroyDrawForNode_(scene, nodeIndex);
		return;
	}

	if (nodeToDraw_.size() < asset.nodes.size())
	{
		nodeToDraw_.resize(asset.nodes.size(), -1);
	}
	if (nodeToDraws_.size() < asset.nodes.size())
	{
		nodeToDraws_.resize(asset.nodes.size());
	}
	if (world_.size() < asset.nodes.size())
	{
		world_.resize(asset.nodes.size(), mathUtils::Mat4(1.0f));
	}

	DestroyDrawForNode_(scene, nodeIndex);

	if (!node.model.empty())
	{
		nodeToDraws_[i] = MakeDrawsForModelNode_(asset, scene, assets, nodeIndex, node);
		nodeToDraw_[i] = nodeToDraws_[i].empty() ? -1 : nodeToDraws_[i].front();
		return;
	}

	DrawItem item{};
	item.mesh = GetOrLoadMeshHandle_(asset, assets, node.mesh);
	item.material = GetMaterialHandle_(node.material);
	item.transform.useMatrix = true;
	item.transform.matrix = world_[i];

	const int drawIndex = static_cast<int>(scene.drawItems.size());
	scene.AddDraw(item);
	if (drawToNode_.size() < scene.drawItems.size())
	{
		drawToNode_.resize(scene.drawItems.size(), -1);
	}
	drawToNode_[static_cast<std::size_t>(drawIndex)] = nodeIndex;
	nodeToDraws_[i] = { drawIndex };
	nodeToDraw_[i] = drawIndex;
}

void DestroySingleDrawIndex_(Scene& scene, int drawIndex)
{
	if (drawIndex < 0)
	{
		return;
	}
	const std::size_t idx = static_cast<std::size_t>(drawIndex);
	if (idx >= scene.drawItems.size())
	{
		return;
	}
	const std::size_t last = scene.drawItems.size() - 1;
	if (idx != last)
	{
		std::swap(scene.drawItems[idx], scene.drawItems[last]);
		const int movedNode = drawToNode_[last];
		drawToNode_[idx] = movedNode;
		if (movedNode >= 0 && static_cast<std::size_t>(movedNode) < nodeToDraws_.size())
		{
			auto& movedDraws = nodeToDraws_[static_cast<std::size_t>(movedNode)];
			for (int& di : movedDraws)
			{
				if (di == static_cast<int>(last))
				{
					di = static_cast<int>(idx);
					break;
				}
			}
			nodeToDraw_[static_cast<std::size_t>(movedNode)] = movedDraws.empty() ? -1 : movedDraws.front();
		}
	}
	scene.drawItems.pop_back();
	drawToNode_.pop_back();
}

void DestroyDrawForNode_(Scene& scene, int nodeIndex)
{
	if (nodeIndex < 0)
	{
		return;
	}
	const std::size_t nodeIdx = static_cast<std::size_t>(nodeIndex);
	if (nodeIdx >= nodeToDraws_.size())
	{
		if (nodeIdx < nodeToDraw_.size())
		{
			nodeToDraw_[nodeIdx] = -1;
		}
		return;
	}

	auto draws = nodeToDraws_[nodeIdx];
	std::sort(draws.begin(), draws.end(), std::greater<int>());
	for (const int di : draws)
	{
		DestroySingleDrawIndex_(scene, di);
	}
	nodeToDraws_[nodeIdx].clear();
	if (nodeIdx < nodeToDraw_.size())
	{
		nodeToDraw_[nodeIdx] = -1;
	}
}

std::vector<int> CollectSubtree_(const LevelAsset& asset, int rootNodeIndex) const
{
	std::vector<int> out;
	if (rootNodeIndex < 0)
		return out;

	const std::size_t nodeCount = asset.nodes.size();
	if (static_cast<std::size_t>(rootNodeIndex) >= nodeCount)
	{
		return out;
	}

	// children adjacency (alive only)
	std::vector<std::vector<int>> children;
	children.resize(nodeCount);

	for (std::size_t i = 0; i < nodeCount; ++i)
	{
		const LevelNode& node = asset.nodes[i];
		if (!node.alive)
			continue;
		if (node.parent < 0)
			continue;

		const std::size_t p = static_cast<std::size_t>(node.parent);
		if (p >= nodeCount)
			continue;
		if (!asset.nodes[p].alive)
			continue;

		children[p].push_back(static_cast<int>(i));
	}

	std::vector<int> stack;
	stack.push_back(rootNodeIndex);

	while (!stack.empty())
	{
		const int cur = stack.back();
		stack.pop_back();

		if (cur < 0 || static_cast<std::size_t>(cur) >= nodeCount)
			continue;
		if (!asset.nodes[static_cast<std::size_t>(cur)].alive)
			continue;

		out.push_back(cur);

		for (int ch : children[static_cast<std::size_t>(cur)])
		{
			stack.push_back(ch);
		}
	}

	return out;
}


void RebuildParticleEmitters_(const LevelAsset& asset, Scene& scene)
{
	scene.particles.clear();
	scene.particleEmitters.clear();
	particleEmitterToSceneEmitter_.clear();
	particleEmitterToSceneEmitter_.reserve(asset.particleEmitters.size());

	for (const ParticleEmitter& emitter : asset.particleEmitters)
	{
		scene.AddParticleEmitter(emitter);
		particleEmitterToSceneEmitter_.push_back(static_cast<int>(scene.particleEmitters.size() - 1));
	}
}

void RemoveParticlesOwnedByEmitter_(Scene& scene, int emitterIndex)
{
	for (Particle& particle : scene.particles)
	{
		if (particle.ownerEmitter == emitterIndex)
		{
			particle.alive = false;
		}
	}
	scene.particles.erase(
		std::remove_if(scene.particles.begin(), scene.particles.end(), [](const Particle& particle)
			{
				return !particle.alive;
			}),
		scene.particles.end());
}

void ValidateRuntimeMappings_(const LevelAsset& asset, const Scene& scene) const noexcept
{
#ifndef NDEBUG
	auto MatricesNearlyEqual = [](const mathUtils::Mat4& a, const mathUtils::Mat4& b) noexcept
		{
			for (std::size_t row = 0; row < 4; ++row)
			{
				for (std::size_t col = 0; col < 4; ++col)
				{
					if (std::fabs(a(row, col) - b(row, col)) > 1e-4f)
					{
						return false;
					}
				}
			}
			return true;
		};

	assert(nodeToDraw_.size() >= asset.nodes.size());
	assert(nodeToDraws_.size() >= asset.nodes.size());
	assert(nodeToEntity_.size() >= asset.nodes.size());
	assert(world_.size() >= asset.nodes.size());
	assert(drawToNode_.size() == scene.drawItems.size());
	assert(scene.editorSelectedDrawItem == GetNodeDrawIndex(scene.editorSelectedNode));

	for (std::size_t i = 0; i < asset.nodes.size(); ++i)
	{
		const LevelNode& node = asset.nodes[i];
		const EntityHandle entity = nodeToEntity_[i];
		if (!node.alive)
		{
			continue;
		}
		assert(entity == kNullEntity || ecs_.IsEntityValid(entity));
		for (const int drawIndex : nodeToDraws_[i])
		{
			assert(drawIndex >= 0);
			assert(static_cast<std::size_t>(drawIndex) < drawToNode_.size());
			assert(drawToNode_[static_cast<std::size_t>(drawIndex)] == static_cast<int>(i));
			assert(static_cast<std::size_t>(drawIndex) < scene.drawItems.size());
		}
		assert((nodeToDraws_[i].empty() ? -1 : nodeToDraws_[i].front()) == nodeToDraw_[i]);
		if (entity != kNullEntity && ecs_.IsEntityValid(entity))
		{
			WorldTransform worldTransform{};
			assert(ecs_.TryGetWorldTransform(entity, worldTransform));
			assert(MatricesNearlyEqual(worldTransform.world, world_[i]));
		}
	}
#endif
}

void RecomputeWorld_(const LevelAsset& asset)
{
	const std::size_t n = asset.nodes.size();
	world_.resize(n, mathUtils::Mat4(1.0f));

	std::vector<std::uint8_t> state;
	state.resize(n, 0); // 0=unvisited, 1=visiting, 2=done

	auto compute = [&](auto&& self, std::size_t i) -> const mathUtils::Mat4&
		{
			if (state[i] == 2)
				return world_[i];

			if (state[i] == 1)
			{
				// cycle - treat as root
				world_[i] = root_ * asset.nodes[i].transform.ToMatrix();
				state[i] = 2;
				return world_[i];
			}

			state[i] = 1;

			const LevelNode& node = asset.nodes[i];
			if (!node.alive)
			{
				world_[i] = mathUtils::Mat4(1.0f);
				state[i] = 2;
				return world_[i];
			}

			mathUtils::Mat4 parentWorld = root_;
			if (node.parent >= 0)
			{
				const std::size_t p = static_cast<std::size_t>(node.parent);
				if (p < n && asset.nodes[p].alive)
				{
					parentWorld = self(self, p);
				}
			}

			world_[i] = parentWorld * node.transform.ToMatrix();
			state[i] = 2;
			return world_[i];
		};

	for (std::size_t i = 0; i < n; ++i)
	{
		compute(compute, i);
	}
}