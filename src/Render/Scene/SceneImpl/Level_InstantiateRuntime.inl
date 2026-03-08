LevelInstance InstantiateLevel(Scene& scene, AssetManager& assets, BindlessTable&, const LevelAsset& asset, const mathUtils::Mat4& root)
{
	LevelInstance inst;
	inst.root_ = root;

	// Camera
	if (asset.camera)
	{
		scene.camera = *asset.camera;
	}

	// Lights
	for (const auto& l : asset.lights)
	{
		scene.AddLight(l);
	}

	// Particle emitters
	for (const auto& e : asset.particleEmitters)
	{
		scene.AddParticleEmitter(e);
	}

	// Textures: request loads (descriptor indices are resolved later)
	for (const auto& [id, td] : asset.textures)
	{
		if (td.kind == LevelTextureKind::Tex2D)
		{
			TextureProperties p = td.props;
			assets.LoadTextureAsync(id, std::move(p));
		}
		else
		{
			TextureProperties p = td.props;
			if (td.cubeSource == LevelCubeSource::Cross)
			{
				p.cubeFromCross = true;
				assets.LoadTextureAsync(id, std::move(p));
			}
			else if (td.cubeSource == LevelCubeSource::AutoFaces)
			{
				if (!td.preferBase.empty())
				{
					assets.LoadTextureCubeAsync(id, td.baseOrDir, td.preferBase, std::move(p));
				}
				else
				{
					assets.LoadTextureCubeAsync(id, td.baseOrDir, std::move(p));
				}
			}
			else
			{
				assets.LoadTextureCubeAsync(id, td.facePaths, std::move(p));
			}
		}
	}

	// Meshes: request loads
	std::unordered_map<std::string, MeshHandle> meshHandles;
	meshHandles.reserve(asset.meshes.size());
	for (const auto& [id, md] : asset.meshes)
	{
		MeshProperties p{};
		p.filePath = md.path;
		p.debugName = md.debugName;
		meshHandles.emplace(id, assets.LoadMeshAsync(id, std::move(p)));
	}

	// Materials: create in Scene and collect pending texture bindings
	std::unordered_map<std::string, MaterialHandle> materialHandles;
	materialHandles.reserve(asset.materials.size());
	for (const auto& [id, md] : asset.materials)
	{
		MaterialHandle h = scene.CreateMaterial(md.material);
		materialHandles.emplace(id, h);

		for (const auto& [slot, texId] : md.textureBindings)
		{
			PendingMaterialBinding pb;
			pb.material = h;
			pb.textureId = texId;

			if (slot == "albedo") pb.slot = MaterialTextureSlot::Albedo;
			else if (slot == "normal") pb.slot = MaterialTextureSlot::Normal;
			else if (slot == "metalness") pb.slot = MaterialTextureSlot::Metalness;
			else if (slot == "roughness") pb.slot = MaterialTextureSlot::Roughness;
			else if (slot == "ao") pb.slot = MaterialTextureSlot::AO;
			else if (slot == "emissive") pb.slot = MaterialTextureSlot::Emissive;
			else if (slot == "specular" || slot == "spec") pb.slot = MaterialTextureSlot::Specular;
			else if (slot == "gloss" || slot == "glossiness") pb.slot = MaterialTextureSlot::Gloss;
			else
			{
				throw std::runtime_error("Level JSON: unknown material texture slot: " + slot);
			}

			inst.pendingBindings_.push_back(std::move(pb));
		}
	}

	inst.materialHandles_ = materialHandles;

	inst.skyboxTextureId_ = asset.skyboxTexture;

	// Nodes: create draw items
	inst.nodeToDraw_.assign(asset.nodes.size(), -1);
	inst.drawToNode_.clear();
	inst.drawToNode_.reserve(asset.nodes.size());

	// Compute world matrices (handles arbitrary parent order)
	inst.transformsDirty_ = true;
	inst.RecomputeWorld_(asset);
	inst.transformsDirty_ = false;

	for (std::size_t i = 0; i < asset.nodes.size(); ++i)
	{
		const LevelNode& n = asset.nodes[i];
		if (!n.alive)
		{
			continue;
		}

		if (!n.visible)
		{
			continue;
		}
		if (n.mesh.empty())
		{
			continue;
		}

		auto meshIt = meshHandles.find(n.mesh);
		if (meshIt == meshHandles.end())
		{
			throw std::runtime_error("Level JSON: node references unknown meshId: " + n.mesh);
		}

		MaterialHandle mat{};
		if (!n.material.empty())
		{
			auto it = materialHandles.find(n.material);
			if (it == materialHandles.end())
			{
				throw std::runtime_error("Level JSON: node references unknown materialId: " + n.material);
			}
			mat = it->second;
		}

		DrawItem item{};
		item.mesh = meshIt->second;
		item.material = mat;
		item.transform.useMatrix = true;
		item.transform.matrix = inst.world_[i];

		const int drawIndex = static_cast<int>(scene.drawItems.size());
		scene.AddDraw(item);
		inst.nodeToDraw_[i] = drawIndex;
		inst.drawToNode_.push_back(static_cast<int>(i));
	}

	// ------------------------------------------------------------
	// ECS build (hybrid phase): one entity per LevelNode (alive)
	// ------------------------------------------------------------

	inst.ecs_.Clear();
	inst.nodeToEntity_.assign(asset.nodes.size(), kNullEntity);

	for (std::size_t i = 0; i < asset.nodes.size(); ++i)
	{
		const LevelNode& n = asset.nodes[i];
		if (!n.alive)
		{
			continue;
		}

		const EntityHandle e = inst.ecs_.CreateEntity();
		inst.nodeToEntity_[i] = e;

		inst.ecs_.EmplaceNodeData(e, static_cast<int>(i), n.parent, n.transform, inst.world_[i], Flags{ .alive = n.alive, .visible = n.visible });

		// Renderable is optional (node can be non-renderable)
		const int drawIndex = inst.nodeToDraw_[i];
		if (drawIndex >= 0)
		{
			// We can grab handles from scene.drawItems since they were created above
			const DrawItem& di = scene.drawItems[static_cast<std::size_t>(drawIndex)];
			inst.ecs_.EmplaceRenderable(e, Renderable{ .mesh = di.mesh, .material = di.material, .drawIndex = drawIndex });
		}
	}

	inst.SyncEditorRuntimeBindings(asset, scene);
	inst.ValidateRuntimeMappingsDebug(asset, scene);
	return inst;
}