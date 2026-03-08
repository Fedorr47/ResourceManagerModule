// -----------------------------
// Editor/runtime mutation API
// (keeps LevelAsset indices stable via tombstones)
// -----------------------------
void SetRootTransform(const mathUtils::Mat4& root)
{
	root_ = root;
	transformsDirty_ = true;
}

bool IsValidNodeIndex(const LevelAsset& asset, int nodeIndex) const noexcept
{
	return nodeIndex >= 0 && static_cast<std::size_t>(nodeIndex) < asset.nodes.size();
}

bool IsNodeAlive(const LevelAsset& asset, int nodeIndex) const noexcept
{
	if (!IsValidNodeIndex(asset, nodeIndex))
	{
		return false;
	}
	return asset.nodes[static_cast<std::size_t>(nodeIndex)].alive;
}

int GetNodeDrawIndex(int nodeIndex) const noexcept
{
	if (nodeIndex < 0)
	{
		return -1;
	}
	const std::size_t i = static_cast<std::size_t>(nodeIndex);
	if (i >= nodeToDraw_.size())
	{
		return -1;
	}
	return nodeToDraw_[i];
}

int GetNodeIndexFromDrawIndex(int drawIndex) const noexcept
{
	if (drawIndex < 0)
	{
		return -1;
	}
	const std::size_t i = static_cast<std::size_t>(drawIndex);
	if (i >= drawToNode_.size())
	{
		return -1;
	}
	return drawToNode_[i];
}

const LevelWorld& GetLevelWorld() const noexcept
{
	return ecs_;
}

EntityHandle GetNodeEntity(int nodeIndex) const noexcept
{
	return GetEntityForNode_(nodeIndex);
}

const mathUtils::Mat4& GetNodeWorldMatrix(int nodeIndex) const noexcept
{
	static const mathUtils::Mat4 identity{ 1.0f };
	if (nodeIndex < 0)
	{
		return identity;
	}
	const std::size_t i = static_cast<std::size_t>(nodeIndex);
	if (i >= world_.size())
	{
		return identity;
	}
	return world_[i];
}

mathUtils::Mat4 GetParentWorldMatrix(const LevelAsset& asset, int nodeIndex) const noexcept
{
	if (!IsValidNodeIndex(asset, nodeIndex))
	{
		return root_;
	}

	const LevelNode& node = asset.nodes[static_cast<std::size_t>(nodeIndex)];
	if (node.parent < 0)
	{
		return root_;
	}

	const std::size_t parentIndex = static_cast<std::size_t>(node.parent);
	if (parentIndex >= world_.size())
	{
		return root_;
	}

	return world_[parentIndex];
}

mathUtils::Vec3 GetNodeWorldPosition(int nodeIndex) const noexcept
{
	return GetNodeWorldMatrix(nodeIndex)[3].xyz();
}


void RebuildParticleEmitters(Scene& scene, const LevelAsset& asset)
{
	scene.particleEmitters.clear();
	for (const ParticleEmitter& emitter : asset.particleEmitters)
	{
		scene.AddParticleEmitter(emitter);
	}
}

// Create a new node and (optionally) spawn a DrawItem.
// Returns the new node index.
int AddNode(LevelAsset& asset,
	Scene& scene,
	AssetManager& assets,
	std::string_view meshId,
	std::string_view materialId,
	int parentNodeIndex,
	const Transform& localTransform,
	std::string_view name = {})
{
	LevelNode node;
	node.name = std::string(name);
	node.parent = parentNodeIndex;
	node.visible = true;
	node.alive = true;
	node.transform = localTransform;
	node.mesh = std::string(meshId);
	node.material = std::string(materialId);

	asset.nodes.push_back(std::move(node));

	if (nodeToDraw_.size() < asset.nodes.size())
	{
		nodeToDraw_.resize(asset.nodes.size(), -1);
	}
	if (world_.size() < asset.nodes.size())
	{
		world_.resize(asset.nodes.size(), mathUtils::Mat4(1.0f));
	}
	if (nodeToEntity_.size() < asset.nodes.size())
	{
		nodeToEntity_.resize(asset.nodes.size(), kNullEntity);
	}

	const int newIndex = static_cast<int>(asset.nodes.size() - 1);

	EnsureDrawForNode_(asset, scene, assets, newIndex);
	EnsureEntityForNode_(asset, newIndex);
	SyncEntityRenderableForNode_(asset, scene, newIndex);
	SyncEditorRuntimeBindings(asset, scene);
	ValidateRuntimeMappingsDebug(asset, scene);
	transformsDirty_ = true;
	return newIndex;
}

// Delete selected node and all its children. (tombstone - keeps indices stable)
void DeleteSubtree(LevelAsset& asset, Scene& scene, int rootNodeIndex)
{
	if (!IsNodeAlive(asset, rootNodeIndex))
	{
		return;
	}

	const std::vector<int> toDelete = CollectSubtree_(asset, rootNodeIndex);
	for (int idx : toDelete)
	{
		if (!IsNodeAlive(asset, idx))
		{
			continue;
		}
		LevelNode& n = asset.nodes[static_cast<std::size_t>(idx)];
		n.alive = false;
		n.visible = false;
		DestroyDrawForNode_(scene, idx);
		DestroyEntityForNode_(idx);
	}

	SyncEditorRuntimeBindings(asset, scene);
	ValidateRuntimeMappingsDebug(asset, scene);
	transformsDirty_ = true;
}

void SetNodeVisible(LevelAsset& asset, Scene& scene, AssetManager& assets, int nodeIndex, bool visible)
{
	if (!IsNodeAlive(asset, nodeIndex))
		return;

	LevelNode& n = asset.nodes[static_cast<std::size_t>(nodeIndex)];
	n.visible = visible;

	EnsureEntityForNode_(asset, nodeIndex);
	EnsureDrawForNode_(asset, scene, assets, nodeIndex);
	SyncEntityRenderableForNode_(asset, scene, nodeIndex);
	SyncEditorRuntimeBindings(asset, scene);
	ValidateRuntimeMappingsDebug(asset, scene);
}

void SetNodeMesh(LevelAsset& asset, Scene& scene, AssetManager& assets, int nodeIndex, std::string_view meshId)
{
	if (!IsNodeAlive(asset, nodeIndex))
	{
		return;
	}

	LevelNode& n = asset.nodes[static_cast<std::size_t>(nodeIndex)];
	n.mesh = std::string(meshId);

	EnsureEntityForNode_(asset, nodeIndex);
	EnsureDrawForNode_(asset, scene, assets, nodeIndex);
	SyncEntityRenderableForNode_(asset, scene, nodeIndex);
	SyncEditorRuntimeBindings(asset, scene);
	ValidateRuntimeMappingsDebug(asset, scene);
}

void SetNodeMaterial(LevelAsset& asset, Scene& scene, int nodeIndex, std::string_view materialId)
{
	if (!IsNodeAlive(asset, nodeIndex))
		return;

	LevelNode& n = asset.nodes[static_cast<std::size_t>(nodeIndex)];
	n.material = std::string(materialId);

	EnsureEntityForNode_(asset, nodeIndex);

	const int di = GetNodeDrawIndex(nodeIndex);
	if (di >= 0 && static_cast<std::size_t>(di) < scene.drawItems.size())
	{
		scene.drawItems[static_cast<std::size_t>(di)].material = GetMaterialHandle_(materialId);
	}

	SyncEntityRenderableForNode_(asset, scene, nodeIndex);
	SyncEditorRuntimeBindings(asset, scene);
	ValidateRuntimeMappingsDebug(asset, scene);
}

void MarkTransformsDirty() noexcept
{
	transformsDirty_ = true;
}

void SyncEditorRuntimeBindings(const LevelAsset& asset, Scene& scene) const noexcept
{
	auto SanitizeNodeIndex = [&](int& nodeIndex) noexcept
		{
			if (!IsNodeAlive(asset, nodeIndex))
			{
				nodeIndex = -1;
			}
		};

	auto SanitizeSelectedNodes = [&]() noexcept
		{
			// Remove dead/out-of-range nodes.
			auto& sel = scene.editorSelectedNodes;
			std::size_t write = 0;
			for (std::size_t i = 0; i < sel.size(); ++i)
			{
				const int nodeIndex = sel[i];
				if (IsNodeAlive(asset, nodeIndex))
				{
					sel[write++] = nodeIndex;
				}
			}
			sel.resize(write);

			// Deduplicate (keep order stable).
			for (std::size_t i = 0; i < sel.size(); ++i)
			{
				for (std::size_t j = i + 1; j < sel.size();)
				{
					if (sel[j] == sel[i])
					{
						sel.erase(sel.begin() + static_cast<std::vector<int>::difference_type>(j));
						continue;
					}
					++j;
				}
			}
		};

	SanitizeNodeIndex(scene.editorSelectedNode);
	SanitizeNodeIndex(scene.editorReflectionCaptureOwnerNode);
	SanitizeSelectedNodes();

	// Keep primary selection consistent with the selection set.
	if (scene.editorSelectedNode >= 0)
	{
		bool found = false;
		for (const int v : scene.editorSelectedNodes)
		{
			if (v == scene.editorSelectedNode)
			{
				found = true;
				break;
			}
		}
		if (!found)
		{
			// Someone set primary directly (e.g. via UI). Treat it as single selection.
			scene.editorSelectedNodes.clear();
			scene.editorSelectedNodes.push_back(scene.editorSelectedNode);
		}
	}
	else
	{
		// If primary is invalid but we still have a set, pick a new primary.
		if (!scene.editorSelectedNodes.empty())
		{
			scene.editorSelectedNode = scene.editorSelectedNodes.back();
		}
	}

	scene.editorSelectedDrawItem = GetNodeDrawIndex(scene.editorSelectedNode);

	// Build selected draw item list.
	scene.editorSelectedDrawItems.clear();
	scene.editorSelectedDrawItems.reserve(scene.editorSelectedNodes.size());
	for (const int nodeIndex : scene.editorSelectedNodes)
	{
		const int di = GetNodeDrawIndex(nodeIndex);
		if (di >= 0)
		{
			scene.editorSelectedDrawItems.push_back(di);
		}
	}
	scene.editorReflectionCaptureOwnerDrawItem = GetNodeDrawIndex(scene.editorReflectionCaptureOwnerNode);
}

void ValidateRuntimeMappingsDebug(const LevelAsset& asset, const Scene& scene) const noexcept
{
#ifndef NDEBUG
	ValidateRuntimeMappings_(asset, scene);
#endif
}

// Recompute world transforms (with hierarchy) and push to Scene draw items.
void SyncTransformsIfDirty(const LevelAsset& asset, Scene& scene)
{
	if (!transformsDirty_)
		return;

	RecomputeWorld_(asset);

	// Push to Scene + ECS
	const std::size_t ncount = asset.nodes.size();
	if (nodeToDraw_.size() < ncount)
		nodeToDraw_.resize(ncount, -1);
	if (nodeToEntity_.size() < ncount)
		nodeToEntity_.resize(ncount, kNullEntity);

	for (std::size_t i = 0; i < ncount; ++i)
	{
		const LevelNode& n = asset.nodes[i];
		if (!n.alive)
		{
			continue;
		}

		const EntityHandle e = EnsureEntityForNode_(asset, static_cast<int>(i));
		if (e != kNullEntity)
		{
			ecs_.UpsertNodeData(e, static_cast<int>(i), n.parent, n.transform, world_[i], Flags{ .alive = n.alive, .visible = n.visible });
		}

		const int di = nodeToDraw_[i];
		if (di < 0 || static_cast<std::size_t>(di) >= scene.drawItems.size())
		{
			SyncEntityRenderableForNode_(asset, scene, static_cast<int>(i));
			continue;
		}

		DrawItem& item = scene.drawItems[static_cast<std::size_t>(di)];
		item.transform.useMatrix = true;
		item.transform.matrix = world_[i];

		SyncEntityRenderableForNode_(asset, scene, static_cast<int>(i));
	}

	SyncEditorRuntimeBindings(asset, scene);
	ValidateRuntimeMappingsDebug(asset, scene);
	transformsDirty_ = false;
}