std::unordered_map<BatchKey, BatchTemp, hashUtils::BatchKeyHash, BatchKeyEq> mainTmp;
mainTmp.reserve(scene.drawItems.size());

std::vector<InstanceData> transparentInstances;
transparentInstances.reserve(scene.drawItems.size());

std::vector<TransparentTemp> transparentTmp;
transparentTmp.reserve(scene.drawItems.size());

std::vector<InstanceData> planarMirrorInstances;
planarMirrorInstances.reserve(std::min<std::size_t>(scene.drawItems.size(), static_cast<std::size_t>(settings_.planarReflectionMaxMirrors)));

std::vector<PlanarMirrorDraw> planarMirrorDraws;
planarMirrorDraws.reserve(std::min<std::size_t>(scene.drawItems.size(), static_cast<std::size_t>(settings_.planarReflectionMaxMirrors)));

std::vector<SkinnedOpaqueDraw> skinnedOpaqueDraws;
skinnedOpaqueDraws.reserve(scene.GetSkinnedDrawItems().size());

std::vector<mathUtils::Mat4> skinnedPaletteMatrices;

// ---------------- Reflection probe assignment (multi-probe) ----------------
drawItemReflectionProbeIndices_.assign(scene.drawItems.size(), -1);
reflectiveOwnerDrawItems_.clear();
reflectiveOwnerDrawItems_.reserve(scene.drawItems.size());

auto IsReflectionCaptureReceiver = [&scene](int drawItemIndex) -> bool
	{
		if (drawItemIndex < 0 || static_cast<std::size_t>(drawItemIndex) >= scene.drawItems.size())
			return false;

		const DrawItem& di = scene.drawItems[static_cast<std::size_t>(drawItemIndex)];
		if (di.material.id == 0)
			return false;

		const auto& mat = scene.GetMaterial(di.material);
		return mat.envSource == EnvSource::ReflectionCapture;
	};

for (std::size_t i = 0; i < scene.drawItems.size(); ++i)
{
	if (!IsReflectionCaptureReceiver(static_cast<int>(i)))
		continue;

	if (reflectiveOwnerDrawItems_.size() >= kMaxReflectionProbes)
		break;

	const int probeIndex = static_cast<int>(reflectiveOwnerDrawItems_.size());
	reflectiveOwnerDrawItems_.push_back(static_cast<int>(i));
	drawItemReflectionProbeIndices_[i] = probeIndex;
}

EnsureReflectionProbeResources(reflectiveOwnerDrawItems_.size());

// ---- Main packing: opaque (batched) + transparent (sorted per-item) ----
// NOTE: mainTmp is camera-culled (IsVisible), but reflection capture must NOT depend on the camera.
// We therefore build an additional "no-cull" packing for reflection capture / cube atlas.
const bool buildCaptureNoCull = settings_.enableReflectionCapture || settings_.ShowCubeAtlas || settings_.enablePlanarReflections;
std::unordered_map<BatchKey, BatchTemp, hashUtils::BatchKeyHash, BatchKeyEq> captureTmp;
if (buildCaptureNoCull)
{
	captureTmp.reserve(scene.drawItems.size());
}
for (std::size_t drawItemIndex = 0; drawItemIndex < scene.drawItems.size(); ++drawItemIndex)
{
	const auto& item = scene.drawItems[drawItemIndex];
	const rendern::MeshRHI* mesh = item.mesh ? &item.mesh->GetResource() : nullptr;
	if (!mesh || mesh->indexCount == 0)
	{
		continue;
	}

	const mathUtils::Mat4 model = item.transform.ToMatrix();
	// Camera visibility is used only for MAIN/transparent lists.
	// Reflection capture uses a separate no-cull packing (captureTmp).
	const bool visibleInMain = IsVisible(item.mesh.get(), model, cameraFrustum, doFrustumCulling);

	BatchKey key{};
	key.mesh = mesh;

	MaterialParams params{};
	MaterialPerm perm = MaterialPerm::UseShadow;
	std::uint32_t itemEnvSource = 0u;
	if (item.material.id != 0)
	{
		const auto& mat = scene.GetMaterial(item.material);
		itemEnvSource = static_cast<std::uint32_t>(mat.envSource);
		params = mat.params;
		perm = EffectivePerm(mat);
	}
	else
	{
		params.baseColor = { 1,1,1,1 };
		params.shininess = 32.0f;
		params.specStrength = 0.2f;
		params.shadowBias = 0.0f;
		params.albedoDescIndex = 0;
		perm = MaterialPerm::UseShadow;
	}

	key.permBits = static_cast<std::uint32_t>(perm);
	key.envSource = itemEnvSource;
	if (drawItemIndex < drawItemReflectionProbeIndices_.size())
	{
		key.reflectionProbeIndex = drawItemReflectionProbeIndices_[drawItemIndex];
	}

	// IMPORTANT: BatchKey must include material parameters,
	// otherwise different materials get incorrectly merged.
	key.albedoDescIndex = params.albedoDescIndex;
	key.normalDescIndex = params.normalDescIndex;
	key.metalnessDescIndex = params.metalnessDescIndex;
	key.roughnessDescIndex = params.roughnessDescIndex;
	key.aoDescIndex = params.aoDescIndex;
	key.emissiveDescIndex = params.emissiveDescIndex;
	key.specularDescIndex = params.specularDescIndex;
	key.glossDescIndex = params.glossDescIndex;

	key.baseColor = params.baseColor;
	key.shadowBias = params.shadowBias; // texels

	key.metallic = params.metallic;
	key.roughness = params.roughness;
	key.ao = params.ao;
	key.emissiveStrength = params.emissiveStrength;

	// Legacy
	key.shininess = params.shininess;
	key.specStrength = params.specStrength;

	// Instance (ROWS)
	const bool isTransparent = HasFlag(perm, MaterialPerm::Transparent) || (params.baseColor.w < 0.999f);
	const bool isPlanarMirror = HasFlag(perm, MaterialPerm::PlanarMirror);
	InstanceData inst{};
	inst.i0 = model[0];
	inst.i1 = model[1];
	inst.i2 = model[2];
	inst.i3 = model[3];

	// Reflection-capture packing is NO-CULL: add before camera-cull so capture does not depend on the editor camera
	if (buildCaptureNoCull && !isTransparent)
	{
		auto& bucket = captureTmp[key];
		if (bucket.inst.empty())
		{
			bucket.materialHandle = item.material;
			bucket.material = params;
			bucket.reflectionProbeIndex = key.reflectionProbeIndex;
		}
		bucket.inst.push_back(inst);
	}

	// Main pass: camera-culled.
	if (!visibleInMain)
	{
		continue;
	}

	if (isTransparent)
	{
		mathUtils::Vec3 sortPos = item.transform.position;
		const auto& b = item.mesh->GetBounds();
		if (b.sphereRadius > 0.0f)
		{
			const mathUtils::Vec4 wc4 = model * mathUtils::Vec4(b.sphereCenter, 1.0f);
			sortPos = mathUtils::Vec3(wc4.x, wc4.y, wc4.z);
		}
		else
		{
			sortPos = mathUtils::Vec3(model[3].x, model[3].y, model[3].z);
		}

		const mathUtils::Vec3 deltaToCamera = sortPos - camPos;
		const float dist2 = mathUtils::Dot(deltaToCamera, deltaToCamera);
		const std::uint32_t localOff = static_cast<std::uint32_t>(transparentInstances.size());
		transparentInstances.push_back(inst);
		transparentTmp.push_back(TransparentTemp{ mesh, params, item.material, localOff, dist2 });

		continue;
	}

	if (settings_.enablePlanarReflections && isPlanarMirror && !isTransparent &&
		planarMirrorDraws.size() < static_cast<std::size_t>(settings_.planarReflectionMaxMirrors))
	{
		PlanarMirrorDraw mirror{};
		mirror.mesh = mesh;
		mirror.material = params;
		mirror.materialHandle = item.material;
		mirror.instanceOffset = static_cast<std::uint32_t>(planarMirrorInstances.size());

		const mathUtils::Vec3 worldX = mathUtils::TransformVector(model, mathUtils::Vec3(1.0f, 0.0f, 0.0f));
		const mathUtils::Vec3 worldY = mathUtils::TransformVector(model, mathUtils::Vec3(0.0f, 1.0f, 0.0f));
		mirror.planePoint = mathUtils::TransformPoint(model, mathUtils::Vec3(0.0f, 0.0f, 0.0f));
		mirror.planeNormal = mathUtils::Cross(worldX, worldY);

		if (mathUtils::Length(mirror.planeNormal) > 0.0001f)
		{
			mirror.planeNormal = mathUtils::Normalize(mirror.planeNormal);
			planarMirrorInstances.push_back(inst);
			planarMirrorDraws.push_back(mirror);
		}

		continue;
	}

	auto& bucket = mainTmp[key];
	if (bucket.inst.empty())
	{
		bucket.materialHandle = item.material;
		bucket.material = params; // representative material for this batch
		bucket.reflectionProbeIndex = key.reflectionProbeIndex;
	}
	bucket.inst.push_back(inst);
}

for (std::size_t skinnedDrawIndex = 0; skinnedDrawIndex < scene.GetSkinnedDrawItems().size(); ++skinnedDrawIndex)
{
	const SkinnedDrawItem& item = scene.GetSkinnedDrawItems()[skinnedDrawIndex];
	if (!item.asset)
	{
		continue;
	}
	if (item.asset->mesh.indices.empty() || item.asset->mesh.vertices.empty())
	{
		continue;
	}
	const mathUtils::Mat4 model = item.transform.ToMatrix();
	if (!IsVisible(item.asset.get(), model, cameraFrustum, doFrustumCulling))
	{
		continue;
	}
	MaterialParams params{};
	MaterialPerm perm = MaterialPerm::UseShadow;
	if (item.material.id != 0)
	{
		const auto& mat = scene.GetMaterial(item.material);
		params = mat.params;
		perm = EffectivePerm(mat);
	}
	else
	{
		params.baseColor = { 1,1,1,1 };
	}
	if (HasFlag(perm, MaterialPerm::Transparent) || params.baseColor.w < 0.999f)
	{
		continue;
	}
	if (item.animator.skinMatrices.empty())
	{
		continue;
	}
	SkinnedOpaqueDraw draw{};
	draw.mesh = &GetOrCreateSkinnedMeshRHI(item.asset);
	draw.material = params;
	draw.materialHandle = item.material;
	draw.model = model;
	draw.paletteOffset = static_cast<std::uint32_t>(skinnedPaletteMatrices.size());
	draw.boneCount = static_cast<std::uint32_t>(item.animator.skinMatrices.size());
	draw.sourceSkinnedDrawIndex = static_cast<int>(skinnedDrawIndex);
	skinnedPaletteMatrices.insert(skinnedPaletteMatrices.end(), item.animator.skinMatrices.begin(), item.animator.skinMatrices.end());
	skinnedOpaqueDraws.push_back(draw);
}

std::vector<InstanceData> mainInstances;
mainInstances.reserve(scene.drawItems.size());

std::vector<Batch> mainBatches;
mainBatches.reserve(mainTmp.size());

for (auto& [key, bt] : mainTmp)
{
	if (bt.inst.empty())
	{
		continue;
	}

	Batch batch{};
	batch.mesh = key.mesh;
	batch.materialHandle = bt.materialHandle;
	batch.material = bt.material;
	batch.instanceOffset = static_cast<std::uint32_t>(mainInstances.size());
	batch.instanceCount = static_cast<std::uint32_t>(bt.inst.size());

	batch.reflectionProbeIndex = bt.reflectionProbeIndex;

	mainInstances.insert(mainInstances.end(), bt.inst.begin(), bt.inst.end());
	mainBatches.push_back(batch);
}

// ---- Reflection-capture no-cull packing (opaque) ----
std::vector<InstanceData> captureMainInstancesNoCull;
std::vector<Batch> captureMainBatchesNoCull;

if (buildCaptureNoCull && !captureTmp.empty())
{
	captureMainInstancesNoCull.reserve(scene.drawItems.size());
	captureMainBatchesNoCull.reserve(captureTmp.size());

	for (auto& [key, bt] : captureTmp)
	{
		if (bt.inst.empty())
			continue;

		Batch batch{};
		batch.mesh = key.mesh;
		batch.materialHandle = bt.materialHandle;
		batch.material = bt.material;
		batch.instanceOffset = static_cast<std::uint32_t>(captureMainInstancesNoCull.size());
		batch.instanceCount = static_cast<std::uint32_t>(bt.inst.size());
		batch.reflectionProbeIndex = bt.reflectionProbeIndex;

		captureMainInstancesNoCull.insert(captureMainInstancesNoCull.end(), bt.inst.begin(), bt.inst.end());
		captureMainBatchesNoCull.push_back(batch);
	}
}

// ---- Optional: layered reflection-capture packing (duplicate MAIN instances x6 for cubemap slices) ----
// Layered reflection capture uses SV_RenderTargetArrayIndex in VS and assumes each original instance
// is duplicated 6 times in order (faces 0..5).
std::vector<InstanceData> reflectionInstancesLayered;
std::vector<Batch> reflectionBatchesLayered;

const bool buildLayeredReflectionCapture =
(psoReflectionCaptureLayered_ && !disableReflectionCaptureLayered_) &&
device_.SupportsShaderModel6() && device_.SupportsVPAndRTArrayIndexFromAnyShader();

if (buildLayeredReflectionCapture && !captureMainBatchesNoCull.empty())
{
	constexpr std::uint32_t kFaces = 6u;

	// reserve roughly
	std::size_t totalMainInst = 0;
	for (const Batch& b : captureMainBatchesNoCull)
	{
		totalMainInst += b.instanceCount;
	}

	reflectionInstancesLayered.reserve(totalMainInst * kFaces);
	reflectionBatchesLayered.reserve(captureMainBatchesNoCull.size());

	for (const Batch& b : captureMainBatchesNoCull)
	{
		if (!b.mesh || b.instanceCount == 0)
			continue;

		Batch lb = b;
		lb.instanceOffset = static_cast<std::uint32_t>(reflectionInstancesLayered.size());
		lb.instanceCount = b.instanceCount * kFaces;

		const std::uint32_t begin = b.instanceOffset;
		const std::uint32_t end = begin + b.instanceCount;

		for (std::uint32_t i = begin; i < end; ++i)
		{
			const InstanceData& inst = captureMainInstancesNoCull[i];
			for (std::uint32_t face = 0; face < kFaces; ++face)
			{
				reflectionInstancesLayered.push_back(inst);
			}
		}

		reflectionBatchesLayered.push_back(lb);
	}
}