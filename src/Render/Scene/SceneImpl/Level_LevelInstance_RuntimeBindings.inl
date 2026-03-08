// -----------------------------
// Runtime: descriptor management
// -----------------------------
void ResolveTextureBindings(AssetManager& assets, BindlessTable& bindless, Scene& scene)
{
	ResourceManager& rm = assets.GetResourceManager();


	// Materials
	for (auto& pb : pendingBindings_)
	{
		rhi::TextureDescIndex idx = GetOrCreateTextureDesc_(rm, bindless, pb.textureId);
		if (idx == 0)
		{
			continue;
		}

		Material& m = scene.GetMaterial(pb.material);
		switch (pb.slot)
		{
		case MaterialTextureSlot::Albedo:    m.params.albedoDescIndex = idx; break;
		case MaterialTextureSlot::Normal:    m.params.normalDescIndex = idx; break;
		case MaterialTextureSlot::Metalness: m.params.metalnessDescIndex = idx; break;
		case MaterialTextureSlot::Roughness: m.params.roughnessDescIndex = idx; break;
		case MaterialTextureSlot::AO:        m.params.aoDescIndex = idx; break;
		case MaterialTextureSlot::Emissive:  m.params.emissiveDescIndex = idx; break;
		case MaterialTextureSlot::Specular:  m.params.specularDescIndex = idx; break;
		case MaterialTextureSlot::Gloss:     m.params.glossDescIndex = idx; break;
		}
	}

	// Skybox
	if (skyboxTextureId_)
	{
		rhi::TextureDescIndex idx = GetOrCreateTextureDesc_(rm, bindless, *skyboxTextureId_);
		if (idx != 0)
		{
			scene.skyboxDescIndex = idx;
		}
	}

	// Particle emitters
	for (auto& emitter : scene.particleEmitters)
	{
		if (!emitter.textureId.empty())
		{
			emitter.textureDescIndex = GetOrCreateTextureDesc_(rm, bindless, emitter.textureId);
		}
		else
		{
			emitter.textureDescIndex = 0;
		}
	}
}

void FreeDescriptors(BindlessTable& bindless) noexcept
{
	for (auto& [_, idx] : textureDesc_)
	{
		if (idx != 0)
		{
			bindless.UnregisterTexture(idx);
		}
	}
	textureDesc_.clear();
}

// Ensure that a materialId exists as a runtime Scene material handle.
// Useful for editor-created materials or late-added materials.
MaterialHandle EnsureMaterial(LevelAsset& asset, Scene& scene, std::string_view materialId)
{
	if (materialId.empty())
	{
		return {};
	}

	const std::string id{ materialId };
	if (auto it = materialHandles_.find(id); it != materialHandles_.end())
	{
		return it->second;
	}

	auto defIt = asset.materials.find(id);
	if (defIt == asset.materials.end())
	{
		return {};
	}

	MaterialHandle h = scene.CreateMaterial(defIt->second.material);
	materialHandles_.emplace(id, h);

	// Register texture bindings (resolved later as textures upload to GPU).
	for (const auto& [slot, texId] : defIt->second.textureBindings)
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
			throw std::runtime_error("Level: unknown material texture slot: " + slot);
		}

		pendingBindings_.push_back(std::move(pb));
	}

	return h;
}