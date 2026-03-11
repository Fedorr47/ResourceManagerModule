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
	if (i < nodeToDraws_.size() && !nodeToDraws_[i].empty())
	{
		return nodeToDraws_[i].front();
	}
	return nodeToDraw_[i];
}

const std::vector<int>& GetNodeDrawIndices(int nodeIndex) const noexcept
{
	static const std::vector<int> empty;
	if (nodeIndex < 0)
	{
		return empty;
	}
	const std::size_t i = static_cast<std::size_t>(nodeIndex);
	if (i >= nodeToDraws_.size())
	{
		return empty;
	}
	return nodeToDraws_[i];
}

int GetNodeSkinnedDrawIndex(int nodeIndex) const noexcept
{
	if (nodeIndex < 0)
	{
		return -1;
	}
	const std::size_t i = static_cast<std::size_t>(nodeIndex);
	if (i >= nodeToSkinnedDraw_.size())
	{
		return -1;
	}
	return nodeToSkinnedDraw_[i];
}

int GetNodeIndexFromSkinnedDrawIndex(int skinnedDrawIndex) const noexcept
{
	if (skinnedDrawIndex < 0)
	{
		return -1;
	}
	const std::size_t i = static_cast<std::size_t>(skinnedDrawIndex);
	if (i >= skinnedDrawToNode_.size())
	{
		return -1;
	}
	return skinnedDrawToNode_[i];
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


bool IsParticleEmitterAlive(const LevelAsset& asset, int emitterIndex) const noexcept
{
	return emitterIndex >= 0 && static_cast<std::size_t>(emitterIndex) < asset.particleEmitters.size();
}

bool IsValidParticleEmitterIndex(int emitterIndex) const noexcept
{
	return emitterIndex >= 0 && static_cast<std::size_t>(emitterIndex) < particleEmitterToSceneEmitter_.size();
}

std::size_t GetParticleEmitterCount() const noexcept
{
	return particleEmitterToSceneEmitter_.size();
}

int AddParticleEmitter(LevelAsset& asset, Scene& scene, const ParticleEmitter& emitter)
{
	asset.particleEmitters.push_back(emitter);
	RebuildParticleEmitters_(asset, scene);
	SyncEditorRuntimeBindings(asset, scene);
	return static_cast<int>(asset.particleEmitters.size() - 1);
}

void DeleteParticleEmitter(LevelAsset& asset, Scene& scene, int emitterIndex)
{
	if (!IsParticleEmitterAlive(asset, emitterIndex))
	{
		return;
	}
	asset.particleEmitters.erase(asset.particleEmitters.begin() + static_cast<std::vector<ParticleEmitter>::difference_type>(emitterIndex));
	RebuildParticleEmitters_(asset, scene);
	if (scene.editorSelectedParticleEmitter == emitterIndex)
	{
		scene.editorSelectedParticleEmitter = -1;
	}
	else if (scene.editorSelectedParticleEmitter > emitterIndex)
	{
		--scene.editorSelectedParticleEmitter;
	}
	SyncEditorRuntimeBindings(asset, scene);
}

void RebuildParticleEmitters(Scene& scene, const LevelAsset& asset)
{
	RebuildParticleEmitters_(asset, scene);
}

void RestartParticleEmitter(const LevelAsset& asset, Scene& scene, int emitterIndex)
{
	if (!IsParticleEmitterAlive(asset, emitterIndex))
	{
		return;
	}
	ParticleEmitter& runtime = scene.particleEmitters[static_cast<std::size_t>(emitterIndex)];
	runtime = asset.particleEmitters[static_cast<std::size_t>(emitterIndex)];
	scene.RestartParticleEmitter(emitterIndex);
}

void SetParticleEmitterPosition(const LevelAsset& asset, Scene& scene, int emitterIndex, const mathUtils::Vec3& position)
{
	if (!IsParticleEmitterAlive(asset, emitterIndex))
	{
		return;
	}
	if (static_cast<std::size_t>(emitterIndex) >= scene.particleEmitters.size())
	{
		return;
	}
	scene.particleEmitters[static_cast<std::size_t>(emitterIndex)].position = position;
}

void TriggerParticleEmitterBurst(const LevelAsset& asset, Scene& scene, int emitterIndex)
{
	if (!IsParticleEmitterAlive(asset, emitterIndex))
	{
		return;
	}
	if (static_cast<std::size_t>(emitterIndex) >= scene.particleEmitters.size())
	{
		return;
	}
	ParticleEmitter& runtime = scene.particleEmitters[static_cast<std::size_t>(emitterIndex)];
	const ParticleEmitter& authoring = asset.particleEmitters[static_cast<std::size_t>(emitterIndex)];
	for (std::uint32_t i = 0; i < authoring.burstCount; ++i)
	{
		scene.EmitParticleFromEmitter(runtime, emitterIndex);
	}
}

const ParticleEmitter* GetRuntimeParticleEmitter(const Scene& scene, int emitterIndex) const noexcept
{
	if (emitterIndex < 0 || static_cast<std::size_t>(emitterIndex) >= scene.particleEmitters.size())
	{
		return nullptr;
	}
	return &scene.particleEmitters[static_cast<std::size_t>(emitterIndex)];
}

ParticleEmitter* GetRuntimeParticleEmitter(Scene& scene, int emitterIndex) noexcept
{
	if (emitterIndex < 0 || static_cast<std::size_t>(emitterIndex) >= scene.particleEmitters.size())
	{
		return nullptr;
	}
	return &scene.particleEmitters[static_cast<std::size_t>(emitterIndex)];
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
	if (nodeToDraws_.size() < asset.nodes.size())
	{
		nodeToDraws_.resize(asset.nodes.size());
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

void EnsureImportedTexture(
	LevelAsset& asset,
	AssetManager& assets,
	std::string_view textureId,
	std::string_view relativePath,
	bool srgb,
	bool isNormalMap)
{
	if (textureId.empty() || relativePath.empty())
	{
		return;
	}

	LevelTextureDef td{};
	td.kind = LevelTextureKind::Tex2D;
	td.props.dimension = TextureDimension::Tex2D;
	td.props.filePath = std::string(relativePath);
	td.props.srgb = srgb;
	td.props.generateMips = true;
	td.props.flipY = false;
	td.props.isNormalMap = isNormalMap;

	auto& dst = asset.textures[std::string(textureId)];
	dst = std::move(td);

	assets.LoadTextureAsync(textureId, dst.props);
}

void BindImportedTexture(
	LevelAsset& asset,
	AssetManager& assets,
	LevelMaterialDef& md,
	std::string_view modelId,
	std::uint32_t materialIndex,
	std::string_view slotName,
	const std::optional<ImportedMaterialTextureRef>& texRef,
	bool srgb)
{
	if (!texRef || texRef->path.empty())
	{
		return;
	}

	const std::string normalizedSlot = (slotName == "metallic") ? "metalness" : std::string(slotName);
	const bool isNormalMap = (normalizedSlot == "normal");
	const std::string texId =
		std::string(modelId) + "__mat_" + std::to_string(materialIndex) + "__" + normalizedSlot;

	EnsureImportedTexture(asset, assets, texId, texRef->path, srgb, isNormalMap);
	md.textureBindings[normalizedSlot] = texId;
}

std::string ImportSkinnedMaterials(
	LevelAsset& asset,
	Scene& scene,
	AssetManager& assets,
	std::string_view skinnedId,
	std::string_view sourcePath,
	bool flipUVs,
	bool cleanupExistingImportedArtifacts = true)
{
	const std::string assetPrefix = std::string(skinnedId) + "__";
	if (cleanupExistingImportedArtifacts)
	{
		for (auto it = asset.materials.begin(); it != asset.materials.end();)
		{
			it = (it->first.rfind(assetPrefix, 0) == 0) ? asset.materials.erase(it) : std::next(it);
		}
		for (auto it = asset.textures.begin(); it != asset.textures.end();)
		{
			it = (it->first.rfind(assetPrefix, 0) == 0) ? asset.textures.erase(it) : std::next(it);
		}
		for (auto it = materialHandles_.begin(); it != materialHandles_.end();)
		{
			it = (it->first.rfind(assetPrefix, 0) == 0) ? materialHandles_.erase(it) : std::next(it);
		}
		pendingBindings_.erase(
			std::remove_if(
				pendingBindings_.begin(),
				pendingBindings_.end(),
				[&](const PendingMaterialBinding& pb)
				{
					return pb.textureId.rfind(assetPrefix, 0) == 0;
				}),
			pendingBindings_.end());
	}

	const ImportedModelScene imported = LoadAssimpScene(std::string(sourcePath), flipUVs, false, true);
	std::string defaultMaterialId;
	for (std::size_t i = 0; i < imported.materials.size(); ++i)
	{
		const ImportedMaterialInfo& srcMat = imported.materials[i];
		const std::string matId = std::string(skinnedId) + "__mat_" + std::to_string(i);

		LevelMaterialDef md{};
		md.material.params.baseColor = mathUtils::Vec4(1.0f, 1.0f, 1.0f, 1.0f);
		md.material.permFlags |= MaterialPerm::Skinning;
		BindImportedTexture(asset, assets, md, skinnedId, static_cast<std::uint32_t>(i), "albedo", srcMat.baseColor, true);
		BindImportedTexture(asset, assets, md, skinnedId, static_cast<std::uint32_t>(i), "normal", srcMat.normal, false);
		BindImportedTexture(asset, assets, md, skinnedId, static_cast<std::uint32_t>(i), "metallic", srcMat.metallic, false);
		BindImportedTexture(asset, assets, md, skinnedId, static_cast<std::uint32_t>(i), "roughness", srcMat.roughness, false);
		BindImportedTexture(asset, assets, md, skinnedId, static_cast<std::uint32_t>(i), "ao", srcMat.ao, false);
		BindImportedTexture(asset, assets, md, skinnedId, static_cast<std::uint32_t>(i), "emissive", srcMat.emissive, true);
		if (!md.textureBindings.empty())
		{
			md.material.permFlags |= MaterialPerm::UseTex;
		}

		asset.materials[matId] = std::move(md);
		[[maybe_unused]] const MaterialHandle runtimeMat = EnsureMaterial(asset, scene, matId);
		if (defaultMaterialId.empty())
		{
			defaultMaterialId = matId;
		}
	}

	return defaultMaterialId;
}

// Import an FBX/Assimp scene as regular Level nodes + mesh defs.
// Each imported Assimp mesh becomes a dedicated Level mesh entry that points to the same source file
// with a submeshIndex override, so saved JSON remains editable and runtime stays mesh-based.
int ImportModelSceneAsNodes(LevelAsset& asset,
	Scene& scene,
	AssetManager& assets,
	std::string_view modelId,
	int parentNodeIndex = -1,
	bool createMaterialPlaceholders = true,
	bool importSkeletonNodes = false,
	bool cleanupExistingImportedArtifacts = true)
{
	namespace fs = std::filesystem;
	auto modelIt = asset.models.find(std::string(modelId));
	if (modelIt == asset.models.end())
	{
		throw std::runtime_error("Level: unknown modelId for scene import: " + std::string(modelId));
	}

	auto MakeAssetRelativePath = [](const fs::path& absolutePath) -> std::string
		{
			const fs::path assetRoot = corefs::FindAssetRoot();
			std::error_code ec;
			const fs::path rel = fs::relative(absolutePath, assetRoot, ec);
			if (!ec && !rel.empty())
			{
				return rel.generic_string();
			}
			return absolutePath.generic_string();
		};

	if (!modelIt->second.path.empty())
	{
		fs::path modelPath(modelIt->second.path);
		if (!modelPath.is_absolute())
		{
			modelPath = corefs::ResolveAsset(modelPath);
		}
		modelIt->second.path = MakeAssetRelativePath(modelPath);
	}

	if (cleanupExistingImportedArtifacts)
	{
		const std::string assetPrefix = std::string(modelId) + "__";
		for (auto it = asset.meshes.begin(); it != asset.meshes.end();)
		{
			it = (it->first.rfind(assetPrefix, 0) == 0) ? asset.meshes.erase(it) : std::next(it);
		}
		for (auto it = asset.materials.begin(); it != asset.materials.end();)
		{
			it = (it->first.rfind(assetPrefix, 0) == 0) ? asset.materials.erase(it) : std::next(it);
		}
		for (auto it = asset.textures.begin(); it != asset.textures.end();)
		{
			it = (it->first.rfind(assetPrefix, 0) == 0) ? asset.textures.erase(it) : std::next(it);
		}

		for (auto it = materialHandles_.begin(); it != materialHandles_.end();)
		{
			it = (it->first.rfind(assetPrefix, 0) == 0) ? materialHandles_.erase(it) : std::next(it);
		}
		pendingBindings_.erase(
			std::remove_if(
				pendingBindings_.begin(),
				pendingBindings_.end(),
				[&](const PendingMaterialBinding& pb)
				{
					return pb.textureId.rfind(assetPrefix, 0) == 0;
				}),
			pendingBindings_.end());
	}

	const ImportedModelScene imported = LoadAssimpScene(modelIt->second.path, modelIt->second.flipUVs, importSkeletonNodes, true);
	if (imported.nodes.empty())
	{
		return -1;
	}


	if (createMaterialPlaceholders)
	{
		for (std::size_t i = 0; i < imported.materials.size(); ++i)
		{
			const ImportedMaterialInfo& srcMat = imported.materials[i];
			const std::string matId = std::string(modelId) + "__mat_" + std::to_string(i);

			LevelMaterialDef md{};
			md.material.params.baseColor = mathUtils::Vec4(1.0f, 1.0f, 1.0f, 1.0f);
			BindImportedTexture(asset, assets, md, modelId, static_cast<std::uint32_t>(i), "albedo", srcMat.baseColor, true);
			BindImportedTexture(asset, assets, md, modelId, static_cast<std::uint32_t>(i), "normal", srcMat.normal, false);
			BindImportedTexture(asset, assets, md, modelId, static_cast<std::uint32_t>(i), "metallic", srcMat.metallic, false);
			BindImportedTexture(asset, assets, md, modelId, static_cast<std::uint32_t>(i), "roughness", srcMat.roughness, false);
			BindImportedTexture(asset, assets, md, modelId, static_cast<std::uint32_t>(i), "ao", srcMat.ao, false);
			BindImportedTexture(asset, assets, md, modelId, static_cast<std::uint32_t>(i), "emissive", srcMat.emissive, true);
			if (!md.textureBindings.empty())
			{
				md.material.permFlags |= MaterialPerm::UseTex;
			}

			asset.materials[matId] = std::move(md);
			[[maybe_unused]] const MaterialHandle runtimeMat = EnsureMaterial(asset, scene, matId);
		}
	}

	std::vector<int> importedNodeMap(imported.nodes.size(), -1);
	int firstImportedNode = -1;

	for (std::size_t i = 0; i < imported.nodes.size(); ++i)
	{
		const ImportedSceneNode& srcNode = imported.nodes[i];
		int runtimeParent = parentNodeIndex;
		if (srcNode.parent >= 0 && static_cast<std::size_t>(srcNode.parent) < importedNodeMap.size())
		{
			runtimeParent = importedNodeMap[static_cast<std::size_t>(srcNode.parent)];
		}

		const int containerNode = AddNode(asset, scene, assets, "", "", runtimeParent, srcNode.localTransform, srcNode.name);
		importedNodeMap[i] = containerNode;
		if (firstImportedNode < 0)
		{
			firstImportedNode = containerNode;
		}

		for (const std::uint32_t submeshIndex : srcNode.submeshes)
		{
			const std::string meshId = std::string(modelId) + "__mesh_" + std::to_string(submeshIndex);
			LevelMeshDef meshDef{};
			meshDef.path = modelIt->second.path;
			meshDef.debugName = modelIt->second.debugName.empty()
				? (std::string(modelId) + "_mesh_" + std::to_string(submeshIndex))
				: (modelIt->second.debugName + "_mesh_" + std::to_string(submeshIndex));
			meshDef.flipUVs = modelIt->second.flipUVs;
			meshDef.submeshIndex = submeshIndex;
			meshDef.bakeNodeTransforms = false;
			asset.meshes[meshId] = std::move(meshDef);

			std::string materialId;
			if (static_cast<std::size_t>(submeshIndex) < imported.submeshes.size())
			{
				const std::uint32_t materialIndex = imported.submeshes[static_cast<std::size_t>(submeshIndex)].materialIndex;
				materialId = std::string(modelId) + "__mat_" + std::to_string(materialIndex);
			}
			AddNode(asset, scene, assets, meshId, materialId, containerNode, Transform{}, std::string(srcNode.name) + "_mesh_" + std::to_string(submeshIndex));
		}
	}

	SyncEditorRuntimeBindings(asset, scene);
	ValidateRuntimeMappingsDebug(asset, scene);
	transformsDirty_ = true;
	return firstImportedNode;
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
		DestroySkinnedDrawForNode_(scene, idx);
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
	n.model.clear();
	n.skinnedMesh.clear();
	n.animationClip.clear();
	n.animationAutoplay = true;
	n.animationLoop = true;
	n.animationPlayRate = 1.0f;
	n.materialOverrides.clear();

	EnsureEntityForNode_(asset, nodeIndex);
	EnsureDrawForNode_(asset, scene, assets, nodeIndex);
	SyncEntityRenderableForNode_(asset, scene, nodeIndex);
	SyncEditorRuntimeBindings(asset, scene);
	ValidateRuntimeMappingsDebug(asset, scene);
}

void SetNodeModel(LevelAsset& asset, Scene& scene, AssetManager& assets, int nodeIndex, std::string_view modelId)
{
	if (!IsNodeAlive(asset, nodeIndex))
	{
		return;
	}

	LevelNode& n = asset.nodes[static_cast<std::size_t>(nodeIndex)];
	n.model = std::string(modelId);
	n.mesh.clear();
	n.skinnedMesh.clear();
	n.animationClip.clear();
	n.animationAutoplay = true;
	n.animationLoop = true;
	n.animationPlayRate = 1.0f;

	EnsureEntityForNode_(asset, nodeIndex);
	EnsureDrawForNode_(asset, scene, assets, nodeIndex);
	SyncEntityRenderableForNode_(asset, scene, nodeIndex);
	SyncEditorRuntimeBindings(asset, scene);
	ValidateRuntimeMappingsDebug(asset, scene);
}

void SetNodeMaterialOverride(LevelAsset& asset, Scene& scene, AssetManager& assets, int nodeIndex, std::uint32_t submeshIndex, std::string_view materialId)
{
	if (!IsNodeAlive(asset, nodeIndex))
	{
		return;
	}

	LevelNode& n = asset.nodes[static_cast<std::size_t>(nodeIndex)];
	if (materialId.empty())
	{
		n.materialOverrides.erase(submeshIndex);
	}
	else
	{
		n.materialOverrides[submeshIndex] = std::string(materialId);
	}

	EnsureEntityForNode_(asset, nodeIndex);
	EnsureDrawForNode_(asset, scene, assets, nodeIndex);
	SyncEntityRenderableForNode_(asset, scene, nodeIndex);
	SyncEditorRuntimeBindings(asset, scene);
	ValidateRuntimeMappingsDebug(asset, scene);
}


void SetNodeSkinnedMesh(LevelAsset& asset, Scene& scene, AssetManager& assets, int nodeIndex, std::string_view skinnedMeshId)
{
	if (!IsNodeAlive(asset, nodeIndex))
	{
		return;
	}

	LevelNode& n = asset.nodes[static_cast<std::size_t>(nodeIndex)];
	if (n.skinnedMesh != skinnedMeshId)
	{
		n.animationClip.clear();
	}
	if (skinnedMeshId.empty())
	{
		n.animationClip.clear();
		n.animationAutoplay = true;
		n.animationLoop = true;
		n.animationPlayRate = 1.0f;
	}
	else
	{
		n.animationPlayRate = std::max(0.0f, n.animationPlayRate);
		if (n.material.empty())
		{
			auto it = asset.skinnedMeshes.find(std::string(skinnedMeshId));
			if (it != asset.skinnedMeshes.end())
			{
				const std::string defaultMaterialId = ImportSkinnedMaterials(
					asset,
					scene,
					assets,
					it->first,
					it->second.path,
					it->second.flipUVs,
					false);
				if (!defaultMaterialId.empty())
				{
					n.material = defaultMaterialId;
				}
			}
		}
	}
	
	n.skinnedMesh = std::string(skinnedMeshId);
	n.mesh.clear();
	n.model.clear();
	n.materialOverrides.clear();

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
	if (!n.model.empty())
	{
		n.materialOverrides.clear();
	}

	EnsureEntityForNode_(asset, nodeIndex);

	const auto& drawIndices = GetNodeDrawIndices(nodeIndex);
	for (const int di : drawIndices)
	{
		if (di >= 0 && static_cast<std::size_t>(di) < scene.drawItems.size())
		{
			scene.drawItems[static_cast<std::size_t>(di)].material = EnsureMaterial(asset, scene, materialId);
		}
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

	auto SanitizeParticleEmitterIndex = [&](int& emitterIndex) noexcept
		{
			if (!IsParticleEmitterAlive(asset, emitterIndex))
			{
				emitterIndex = -1;
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
	SanitizeParticleEmitterIndex(scene.editorSelectedParticleEmitter);
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

	if (scene.editorSelectedParticleEmitter >= 0)
	{
		scene.editorSelectedNode = -1;
		scene.editorSelectedNodes.clear();
	}

	scene.editorSelectedDrawItem = GetNodeDrawIndex(scene.editorSelectedNode);
	scene.editorSelectedSkinnedDrawItem = GetNodeSkinnedDrawIndex(scene.editorSelectedNode);

	// Build selected draw item lists.
	scene.editorSelectedDrawItems.clear();
	scene.editorSelectedSkinnedDrawItems.clear();
	for (const int nodeIndex : scene.editorSelectedNodes)
	{
		const auto& drawIndices = GetNodeDrawIndices(nodeIndex);
		for (const int di : drawIndices)
		{
			if (di >= 0)
			{
				scene.editorSelectedDrawItems.push_back(di);
			}
		}

		const int skinnedDrawIndex = GetNodeSkinnedDrawIndex(nodeIndex);
		if (skinnedDrawIndex >= 0)
		{
			scene.editorSelectedSkinnedDrawItems.push_back(skinnedDrawIndex);
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
	if (nodeToDraws_.size() < ncount)
		nodeToDraws_.resize(ncount);
	if (nodeToEntity_.size() < ncount)
		nodeToEntity_.resize(ncount, kNullEntity);
	if (nodeToSkinnedDraw_.size() < ncount)
		nodeToSkinnedDraw_.resize(ncount, -1);

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

		const auto& drawIndices = (i < nodeToDraws_.size()) ? nodeToDraws_[i] : std::vector<int>{};
		const int skinnedDrawIndex = (i < nodeToSkinnedDraw_.size()) ? nodeToSkinnedDraw_[i] : -1;
		if (drawIndices.empty())
		{
			if (skinnedDrawIndex >= 0 && static_cast<std::size_t>(skinnedDrawIndex) < scene.skinnedDrawItems.size())
			{
				SkinnedDrawItem& item = scene.skinnedDrawItems[static_cast<std::size_t>(skinnedDrawIndex)];
				item.transform.useMatrix = true;
				item.transform.matrix = world_[i];
			}
			SyncEntityRenderableForNode_(asset, scene, static_cast<int>(i));
			continue;
		}

		for (const int di : drawIndices)
		{
			if (di < 0 || static_cast<std::size_t>(di) >= scene.drawItems.size())
			{
				continue;
			}
			DrawItem& item = scene.drawItems[static_cast<std::size_t>(di)];
			item.transform.useMatrix = true;
			item.transform.matrix = world_[i];
		}

		if (skinnedDrawIndex >= 0 && static_cast<std::size_t>(skinnedDrawIndex) < scene.skinnedDrawItems.size())
		{
			SkinnedDrawItem& item = scene.skinnedDrawItems[static_cast<std::size_t>(skinnedDrawIndex)];
			item.transform.useMatrix = true;
			item.transform.matrix = world_[i];
		}

		SyncEntityRenderableForNode_(asset, scene, static_cast<int>(i));
	}

	SyncEditorRuntimeBindings(asset, scene);
	ValidateRuntimeMappingsDebug(asset, scene);
	transformsDirty_ = false;
}

std::size_t GetSkinnedDrawCount(const Scene& scene) const noexcept
{
	return scene.skinnedDrawItems.size();
}

SkinnedDrawItem* GetSkinnedDrawItem(Scene& scene, int skinnedDrawIndex) noexcept
{
	if (skinnedDrawIndex < 0 || static_cast<std::size_t>(skinnedDrawIndex) >= scene.skinnedDrawItems.size())
	{
		return nullptr;
	}
	return &scene.skinnedDrawItems[static_cast<std::size_t>(skinnedDrawIndex)];
}

const SkinnedDrawItem* GetSkinnedDrawItem(const Scene& scene, int skinnedDrawIndex) const noexcept
{
	if (skinnedDrawIndex < 0 || static_cast<std::size_t>(skinnedDrawIndex) >= scene.skinnedDrawItems.size())
	{
		return nullptr;
	}
	return &scene.skinnedDrawItems[static_cast<std::size_t>(skinnedDrawIndex)];
}
