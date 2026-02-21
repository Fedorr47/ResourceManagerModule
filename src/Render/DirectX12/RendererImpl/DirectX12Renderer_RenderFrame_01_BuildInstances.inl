			// ---------------- Build instance draw lists (ONE upload) ----------------
			// We build two packings:
			//   1) Shadow packing: per-mesh batching (used by directional/spot/point shadow passes)
			//   2) Main packing: per-(mesh+material params) batching (used by MainPass)
			//
			// Then we concatenate them into a single instanceBuffer_ update.
			// ---- Shadow packing (per mesh) ----
			std::unordered_map<const rendern::MeshRHI*, std::vector<InstanceData>> shadowTmp;
			shadowTmp.reserve(scene.drawItems.size());
			std::uint32_t envSource = 0u;

			for (const auto& item : scene.drawItems)
			{
				const rendern::MeshRHI* mesh = item.mesh ? &item.mesh->GetResource() : nullptr;
				if (!mesh || mesh->indexCount == 0)
				{
					continue;
				}
				const mathUtils::Mat4 model = item.transform.ToMatrix();
				// IMPORTANT: exclude alpha-blended objects from shadow casting
				MaterialParams params{};
				envSource = 0u;
				MaterialPerm perm = MaterialPerm::UseShadow;
				std::uint32_t itemEnvSource = 0u;

				if (item.material.id != 0)
				{
					const auto& mat = scene.GetMaterial(item.material);
					itemEnvSource = static_cast<std::uint32_t>(mat.envSource);
					params = mat.params;
					perm = EffectivePerm(mat);
					envSource = static_cast<std::uint32_t>(mat.envSource);
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

				const bool isTransparent = HasFlag(perm, MaterialPerm::Transparent) || (params.baseColor.w < 0.999f);
				if (isTransparent)
				{
					continue;
				}

				InstanceData inst{};
				inst.i0 = model[0];
				inst.i1 = model[1];
				inst.i2 = model[2];
				inst.i3 = model[3];

				shadowTmp[mesh].push_back(inst);
			}

			std::vector<InstanceData> shadowInstances;
			std::vector<ShadowBatch> shadowBatches;
			shadowInstances.reserve(scene.drawItems.size());
			shadowBatches.reserve(shadowTmp.size());

			{
				std::vector<const rendern::MeshRHI*> meshes;
				meshes.reserve(shadowTmp.size());
				for (auto& [shadowMesh, _] : shadowTmp)
				{
					meshes.push_back(shadowMesh);
				}
				std::sort(meshes.begin(), meshes.end());

				for (const rendern::MeshRHI* mesh : meshes)
				{
					auto& vec = shadowTmp[mesh];
					if (!mesh || vec.empty())
					{
						continue;
					}

					ShadowBatch shadowBatch{};
					shadowBatch.mesh = mesh;
					shadowBatch.instanceOffset = static_cast<std::uint32_t>(shadowInstances.size());
					shadowBatch.instanceCount = static_cast<std::uint32_t>(vec.size());

					shadowInstances.insert(shadowInstances.end(), vec.begin(), vec.end());
					shadowBatches.push_back(shadowBatch);
				}
			}

			// ---- Optional: layered point-shadow packing (duplicate instances x6 for cubemap slices) ----
			// Layered point shadow renders into a Texture2DArray(6) in a single pass and uses
			// SV_RenderTargetArrayIndex in VS. The shader assumes instance data is duplicated 6 times:
			// for each original instance we emit faces 0..5 in order.
			std::vector<InstanceData> shadowInstancesLayered;
			std::vector<ShadowBatch> shadowBatchesLayered;
			const bool buildLayeredPointShadow = (psoPointShadowLayered_ && !disablePointShadowLayered_) &&
				device_.SupportsShaderModel6() && device_.SupportsVPAndRTArrayIndexFromAnyShader();
			if (buildLayeredPointShadow && !shadowBatches.empty())
			{
				constexpr std::uint32_t kPointShadowFaces = 6u;
				shadowInstancesLayered.reserve(static_cast<std::size_t>(shadowInstances.size()) * kPointShadowFaces);
				shadowBatchesLayered.reserve(shadowBatches.size());

				for (const ShadowBatch& sb : shadowBatches)
				{
					if (!sb.mesh || sb.instanceCount == 0)
					{
						continue;
					}

					ShadowBatch lb{};
					lb.mesh = sb.mesh;
					lb.instanceOffset = static_cast<std::uint32_t>(shadowInstancesLayered.size());
					lb.instanceCount = sb.instanceCount * kPointShadowFaces;

					const std::uint32_t begin = sb.instanceOffset;
					const std::uint32_t end = begin + sb.instanceCount;
					for (std::uint32_t i = begin; i < end; ++i)
					{
						const InstanceData& inst = shadowInstances[i];
						for (std::uint32_t face = 0; face < kPointShadowFaces; ++face)
						{
							shadowInstancesLayered.push_back(inst);
						}
					}

					shadowBatchesLayered.push_back(lb);
				}
			}

			// ---- Main packing: opaque (batched) + transparent (sorted per-item) ----
			std::unordered_map<BatchKey, BatchTemp, BatchKeyHash, BatchKeyEq> mainTmp;
			mainTmp.reserve(scene.drawItems.size());

			std::vector<InstanceData> transparentInstances;
			transparentInstances.reserve(scene.drawItems.size());

			std::vector<TransparentTemp> transparentTmp;
			transparentTmp.reserve(scene.drawItems.size());

			for (const auto& item : scene.drawItems)
			{
				const rendern::MeshRHI* mesh = item.mesh ? &item.mesh->GetResource() : nullptr;
				if (!mesh || mesh->indexCount == 0)
				{
					continue;
				}

				const mathUtils::Mat4 model = item.transform.ToMatrix();
				if (!IsVisible(item.mesh.get(), model))
				{
					continue;
				}

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

				// IMPORTANT: BatchKey must include material parameters,
				// otherwise different materials get incorrectly merged.
				key.albedoDescIndex = params.albedoDescIndex;
				key.normalDescIndex = params.normalDescIndex;
				key.metalnessDescIndex = params.metalnessDescIndex;
				key.roughnessDescIndex = params.roughnessDescIndex;
				key.aoDescIndex = params.aoDescIndex;
				key.emissiveDescIndex = params.emissiveDescIndex;

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
				InstanceData inst{};
				inst.i0 = model[0];
				inst.i1 = model[1];
				inst.i2 = model[2];
				inst.i3 = model[3];

				const bool isTransparent = HasFlag(perm, MaterialPerm::Transparent) || (params.baseColor.w < 0.999f);
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

				auto& bucket = mainTmp[key];
				if (bucket.inst.empty())
				{
					bucket.materialHandle = item.material;
					bucket.material = params; // representative material for this batch
				}
				bucket.inst.push_back(inst);
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

				mainInstances.insert(mainInstances.end(), bt.inst.begin(), bt.inst.end());
				mainBatches.push_back(batch);
			}

			// ---- Optional: layered reflection-capture packing (duplicate MAIN instances x6 for cubemap slices) ----
			// Layered reflection capture uses SV_RenderTargetArrayIndex in VS and assumes each original instance
			// is duplicated 6 times in order (faces 0..5).
			std::vector<InstanceData> reflectionInstancesLayered;
			std::vector<Batch> reflectionBatchesLayered;

			const bool buildLayeredReflectionCapture =
				(psoReflectionCaptureLayered_ && !disableReflectionCaptureLayered_) &&
				device_.SupportsShaderModel6() && device_.SupportsVPAndRTArrayIndexFromAnyShader();

			if (buildLayeredReflectionCapture && !mainBatches.empty())
			{
				constexpr std::uint32_t kFaces = 6u;

				// reserve roughly
				std::size_t totalMainInst = 0;
				for (const Batch& b : mainBatches) totalMainInst += b.instanceCount;

				reflectionInstancesLayered.reserve(totalMainInst * kFaces);
				reflectionBatchesLayered.reserve(mainBatches.size());

				for (const Batch& b : mainBatches)
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
						const InstanceData& inst = mainInstances[i];
						for (std::uint32_t face = 0; face < kFaces; ++face)
						{
							reflectionInstancesLayered.push_back(inst);
						}
					}

					reflectionBatchesLayered.push_back(lb);
				}
			}

			// ---- Combine and upload once ----
			auto AlignUpU32 = [](std::uint32_t v, std::uint32_t a) -> std::uint32_t
				{
					return (v + (a - 1u)) / a * a;
				};
			const std::uint32_t shadowBase = 0;
			const std::uint32_t mainBase = static_cast<std::uint32_t>(shadowInstances.size());
			const std::uint32_t transparentBase = static_cast<std::uint32_t>(shadowInstances.size() + mainInstances.size());

			const std::uint32_t transparentEnd =
				transparentBase + static_cast<std::uint32_t>(transparentInstances.size());
			const std::uint32_t layeredShadowBase = AlignUpU32(transparentEnd, 6u);
			const std::uint32_t layeredReflectionBase =
				AlignUpU32(layeredShadowBase + static_cast<std::uint32_t>(shadowInstancesLayered.size()), 6u);


			for (auto& sbatch : shadowBatches)
			{
				sbatch.instanceOffset += shadowBase;
			}
			for (auto& mbatch : mainBatches)
			{
				mbatch.instanceOffset += mainBase;
			}
			for (auto& lbatch : shadowBatchesLayered)
			{
				lbatch.instanceOffset += layeredShadowBase;
			}
			for (auto& rbatch : reflectionBatchesLayered)
			{
				rbatch.instanceOffset += layeredReflectionBase;
			}

			std::vector<TransparentDraw> transparentDraws;
			transparentDraws.reserve(transparentTmp.size());
			for (const auto& transparentInst : transparentTmp)
			{
				TransparentDraw transparentDraw{};
				transparentDraw.mesh = transparentInst.mesh;
				transparentDraw.material = transparentInst.material;
				transparentDraw.materialHandle = transparentInst.materialHandle;
				transparentDraw.instanceOffset = transparentBase + transparentInst.localInstanceOffset;
				transparentDraw.dist2 = transparentInst.dist2;
				transparentDraws.push_back(transparentDraw);
			}

			std::sort(transparentDraws.begin(), transparentDraws.end(),
				[](const TransparentDraw& first, const TransparentDraw& second)
				{
					return first.dist2 > second.dist2; // far -> near
				});

			std::vector<InstanceData> combinedInstances;
			const std::uint32_t finalCount =
				layeredReflectionBase + (std::uint32_t)reflectionInstancesLayered.size();

			combinedInstances.clear();
			combinedInstances.reserve(finalCount);

			// 1) normal groups
			combinedInstances.insert(combinedInstances.end(), shadowInstances.begin(), shadowInstances.end());
			combinedInstances.insert(combinedInstances.end(), mainInstances.begin(), mainInstances.end());
			combinedInstances.insert(combinedInstances.end(), transparentInstances.begin(), transparentInstances.end());

			// 2) pad up to layeredShadowBase (between transparent and layered shadow)
			if (combinedInstances.size() < layeredShadowBase)
				combinedInstances.resize(layeredShadowBase);

			// 3) layered shadow
			combinedInstances.insert(combinedInstances.end(),
				shadowInstancesLayered.begin(), shadowInstancesLayered.end());

			// 4) pad up to layeredReflectionBase (between layered shadow and layered reflection)
			if (combinedInstances.size() < layeredReflectionBase)
				combinedInstances.resize(layeredReflectionBase);

			// 5) layered reflection
			combinedInstances.insert(combinedInstances.end(),
				reflectionInstancesLayered.begin(), reflectionInstancesLayered.end());

			assert(shadowBase == 0u);
			assert(mainBase == shadowInstances.size());
			assert(transparentBase == shadowInstances.size() + mainInstances.size());
			assert(layeredShadowBase >= transparentBase + transparentInstances.size());
			assert(layeredReflectionBase >= layeredShadowBase + shadowInstancesLayered.size());
			assert(combinedInstances.size() == finalCount);

			const std::uint32_t instStride = static_cast<std::uint32_t>(sizeof(InstanceData));

			if (!combinedInstances.empty())
			{
				const std::size_t bytes = combinedInstances.size() * sizeof(InstanceData);
				if (bytes > instanceBufferSizeBytes_)
				{
					throw std::runtime_error("DX12Renderer: instance buffer overflow (increase instanceBufferSizeBytes_)");
				}
				device_.UpdateBuffer(instanceBuffer_, std::as_bytes(std::span{ combinedInstances }));
			}

			if (settings_.debugPrintDrawCalls)
			{
				static std::uint32_t frame = 0;
				if ((++frame % 60u) == 0u)
				{
					std::cout << "[DX12] MainPass draw calls: " << mainBatches.size()
						<< " (instances main: " << mainInstances.size()
						<< ", shadow: " << shadowInstances.size() << ")"
						<< " | DepthPrepass: " << (settings_.enableDepthPrepass ? "ON" : "OFF")
						<< " (draw calls: " << shadowBatches.size() << ")\n";
				}
			}

