			// ---------------- Main pass (swapchain) ----------------
			rhi::ClearDesc clearDesc{};
			clearDesc.clearColor = true;
			clearDesc.clearDepth = !doDepthPrepass; // if we pre-filled depth, don't wipe it here
			clearDesc.color = { 0.1f, 0.1f, 0.1f, 1.0f };

			graph.AddSwapChainPass("MainPass", clearDesc,
				[this, &scene,
				shadowRG,
				dirLightViewProj,
				lightCount,
				spotShadows,
				pointShadows,
				mainBatches,
				instStride,
				transparentDraws,
				doDepthPrepass,
				imguiDrawData](renderGraph::PassContext& ctx)
				{
					const auto extent = ctx.passExtent;

					ctx.commandList.SetViewport(0, 0,
						static_cast<int>(extent.width),
						static_cast<int>(extent.height));

					// If we ran a depth prepass, keep depth read-only in the main pass.
					ctx.commandList.SetState(doDepthPrepass ? mainAfterPreDepthState_ : state_);

					const float aspect = extent.height
						? (static_cast<float>(extent.width) / static_cast<float>(extent.height))
						: 1.0f;

					const mathUtils::Mat4 proj = mathUtils::PerspectiveRH_ZO(mathUtils::DegToRad(scene.camera.fovYDeg), aspect, scene.camera.nearZ, scene.camera.farZ);
					const mathUtils::Mat4 view = mathUtils::LookAt(scene.camera.position, scene.camera.target, scene.camera.up);
					const mathUtils::Vec3 camPosLocal = scene.camera.position;

					const mathUtils::Vec3 camFLocal = mathUtils::Normalize(scene.camera.target - scene.camera.position);
					const mathUtils::Mat4 viewProj = proj * view;


					// --- Skybox draw ---
					{
						if (scene.skyboxDescIndex != 0)
						{
							mathUtils::Mat4 viewNoTranslation = view;
							viewNoTranslation[3] = mathUtils::Vec4(0, 0, 0, 1);

							const mathUtils::Mat4 viewProjSkybox = proj * viewNoTranslation;
							const mathUtils::Mat4 viewProjSkyboxTranspose = mathUtils::Transpose(viewProjSkybox);

							SkyboxConstants skyboxConstants{};
							std::memcpy(skyboxConstants.uViewProj.data(), mathUtils::ValuePtr(viewProjSkyboxTranspose), sizeof(float) * 16);

							ctx.commandList.SetState(skyboxState_);
							ctx.commandList.BindPipeline(psoSkybox_);
							ctx.commandList.BindTextureDesc(0, scene.skyboxDescIndex);

							ctx.commandList.BindInputLayout(skyboxMesh_.layout);
							ctx.commandList.BindVertexBuffer(0, skyboxMesh_.vertexBuffer, skyboxMesh_.vertexStrideBytes, 0);
							ctx.commandList.BindIndexBuffer(skyboxMesh_.indexBuffer, skyboxMesh_.indexType, 0);

							ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &skyboxConstants, 1 }));
							ctx.commandList.DrawIndexed(skyboxMesh_.indexCount, skyboxMesh_.indexType, 0, 0);

							ctx.commandList.SetState(doDepthPrepass ? mainAfterPreDepthState_ : state_);
						}
					}

					// Bind directional shadow map at slot 1 (t1)
					{
						const auto shadowTex = ctx.resources.GetTexture(shadowRG);
						ctx.commandList.BindTexture2D(1, shadowTex);
					}

					// Bind Spot shadow maps at t3..t6 and Point shadow cubemaps at t7..t10.
					for (std::size_t spotShadowIndex = 0; spotShadowIndex < spotShadows.size(); ++spotShadowIndex)
					{
						const auto tex = ctx.resources.GetTexture(spotShadows[spotShadowIndex].tex);
						ctx.commandList.BindTexture2D(3 + static_cast<std::uint32_t>(spotShadowIndex), tex);
					}
					for (std::size_t pointShadowIndex = 0; pointShadowIndex < pointShadows.size(); ++pointShadowIndex)
					{
						const auto tex = ctx.resources.GetTexture(pointShadows[pointShadowIndex].cube);
						ctx.commandList.BindTexture2DArray(7 + static_cast<std::uint32_t>(pointShadowIndex), tex);
					}

					// Bind shadow metadata SB at t11
					ctx.commandList.BindStructuredBufferSRV(11, shadowDataBuffer_);

					// Bind lights (t2 StructuredBuffer SRV)
					ctx.commandList.BindStructuredBufferSRV(2, lightsBuffer_);

					constexpr std::uint32_t kFlagUseTex = 1u << 0;
					constexpr std::uint32_t kFlagUseShadow = 1u << 1;
					constexpr std::uint32_t kFlagUseNormal = 1u << 2;
					constexpr std::uint32_t kFlagUseMetalTex = 1u << 3;
					constexpr std::uint32_t kFlagUseRoughTex = 1u << 4;
					constexpr std::uint32_t kFlagUseAOTex = 1u << 5;
					constexpr std::uint32_t kFlagUseEmissiveTex = 1u << 6;
					constexpr std::uint32_t kFlagUseEnv = 1u << 7;
					constexpr std::uint32_t kFlagEnvFlipZ = 1u << 8;
					constexpr std::uint32_t kFlagEnvForceMip0 = 1u << 9;

					for (const Batch& batch : mainBatches)
					{
						if (!batch.mesh || batch.instanceCount == 0)
						{
							continue;
						}

						MaterialPerm perm = MaterialPerm::UseShadow;
						if (batch.materialHandle.id != 0)
						{
							perm = EffectivePerm(scene.GetMaterial(batch.materialHandle));
						}
						else
						{
							// Fallback: infer only from params.
							if (batch.material.albedoDescIndex != 0)
							{
								perm = perm | MaterialPerm::UseTex;
							}
						}

						const bool useTex = HasFlag(perm, MaterialPerm::UseTex);
						const bool useShadow = HasFlag(perm, MaterialPerm::UseShadow);

						ctx.commandList.BindPipeline(MainPipelineFor(perm));
						ctx.commandList.BindTextureDesc(0, batch.material.albedoDescIndex);
						ctx.commandList.BindTextureDesc(12, batch.material.normalDescIndex);
						ctx.commandList.BindTextureDesc(13, batch.material.metalnessDescIndex);
						ctx.commandList.BindTextureDesc(14, batch.material.roughnessDescIndex);
						ctx.commandList.BindTextureDesc(15, batch.material.aoDescIndex);
						ctx.commandList.BindTextureDesc(16, batch.material.emissiveDescIndex);
						rhi::TextureDescIndex envDescIndex = scene.skyboxDescIndex;
						if (settings_.enableReflectionCapture && (reflectionCubeDescIndex_ != 0) && (batch.materialHandle.id != 0))
						{
							const auto& mat = scene.GetMaterial(batch.materialHandle);
							if (mat.envSource == EnvSource::ReflectionCapture)
							{
								envDescIndex = reflectionCubeDescIndex_;
							}
						}
						ctx.commandList.BindTextureDesc(17, envDescIndex);

						std::uint32_t flags = 0;
						if (useTex)
						{
							flags |= kFlagUseTex;
						}
						if (useShadow)
						{
							flags |= kFlagUseShadow;
						}
						if (batch.material.normalDescIndex != 0)
						{
							flags |= kFlagUseNormal;
						}
						if (batch.material.metalnessDescIndex != 0)
						{
							flags |= kFlagUseMetalTex;
						}
						if (batch.material.roughnessDescIndex != 0)
						{
							flags |= kFlagUseRoughTex;
						}
						if (batch.material.aoDescIndex != 0)
						{
							flags |= kFlagUseAOTex;
						}
						if (batch.material.emissiveDescIndex != 0)
						{
							flags |= kFlagUseEmissiveTex;
						}
						if (envDescIndex != 0)
						{
							flags |= kFlagUseEnv;
							if (envDescIndex == scene.skyboxDescIndex)
							{
								flags |= kFlagEnvFlipZ;
							}
						}
						if (settings_.enableReflectionCapture && (reflectionCubeDescIndex_ != 0) && (envDescIndex == reflectionCubeDescIndex_))
						{
							// Dynamic reflection capture cubemap: render mip0 only -> force mip0 sampling in shader.
							flags |= kFlagEnvForceMip0;
						}

						PerBatchConstants constants{};
						const mathUtils::Mat4 viewProjT = mathUtils::Transpose(viewProj);
						const mathUtils::Mat4 dirVP_T = mathUtils::Transpose(dirLightViewProj);

						std::memcpy(constants.uViewProj.data(), mathUtils::ValuePtr(viewProjT), sizeof(float) * 16);
						std::memcpy(constants.uLightViewProj.data(), mathUtils::ValuePtr(dirVP_T), sizeof(float) * 16);

						constants.uCameraAmbient = { camPosLocal.x, camPosLocal.y, camPosLocal.z, 0.22f };
						constants.uCameraForward = { camFLocal.x, camFLocal.y, camFLocal.z, 0.0f };
						constants.uBaseColor = { batch.material.baseColor.x, batch.material.baseColor.y, batch.material.baseColor.z, batch.material.baseColor.w };

						const float materialBiasTexels = batch.material.shadowBias;
						constants.uMaterialFlags = { 0.0f, 0.0f, materialBiasTexels, AsFloatBits(flags) };

						constants.uPbrParams = { batch.material.metallic, batch.material.roughness, batch.material.ao, batch.material.emissiveStrength };

						constants.uCounts = {
							static_cast<float>(lightCount),
							static_cast<float>(spotShadows.size()),
							static_cast<float>(pointShadows.size()),
							0.0f
						};

						constants.uShadowBias = {
							settings_.dirShadowBaseBiasTexels,
							settings_.spotShadowBaseBiasTexels,
							settings_.pointShadowBaseBiasTexels,
							settings_.shadowSlopeScaleTexels
						};

						// IA (instanced)
						ctx.commandList.BindInputLayout(batch.mesh->layoutInstanced);
						ctx.commandList.BindVertexBuffer(0, batch.mesh->vertexBuffer, batch.mesh->vertexStrideBytes, 0);
						ctx.commandList.BindVertexBuffer(1, instanceBuffer_, instStride, batch.instanceOffset * instStride);
						ctx.commandList.BindIndexBuffer(batch.mesh->indexBuffer, batch.mesh->indexType, 0);

						ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &constants, 1 }));
						ctx.commandList.DrawIndexed(batch.mesh->indexCount, batch.mesh->indexType, 0, 0, batch.instanceCount, 0);
					}

					if (!transparentDraws.empty())
					{
						ctx.commandList.SetState(transparentState_);

						for (const TransparentDraw& batchTransparent : transparentDraws)
						{
							if (!batchTransparent.mesh)
							{
								continue;
							}

							MaterialPerm perm = MaterialPerm::UseShadow;
							if (batchTransparent.materialHandle.id != 0)
							{
								perm = EffectivePerm(scene.GetMaterial(batchTransparent.materialHandle));
							}
							else
							{
								if (batchTransparent.material.albedoDescIndex != 0)
								{
									perm = perm | MaterialPerm::UseTex;
								}
							}

							const bool useTex = HasFlag(perm, MaterialPerm::UseTex);
							const bool useShadow = HasFlag(perm, MaterialPerm::UseShadow);

							ctx.commandList.BindPipeline(MainPipelineFor(perm));
							ctx.commandList.BindTextureDesc(0, batchTransparent.material.albedoDescIndex);
							ctx.commandList.BindTextureDesc(12, batchTransparent.material.normalDescIndex);
							ctx.commandList.BindTextureDesc(13, batchTransparent.material.metalnessDescIndex);
							ctx.commandList.BindTextureDesc(14, batchTransparent.material.roughnessDescIndex);
							ctx.commandList.BindTextureDesc(15, batchTransparent.material.aoDescIndex);
							ctx.commandList.BindTextureDesc(16, batchTransparent.material.emissiveDescIndex);
							rhi::TextureDescIndex envDescIndex = scene.skyboxDescIndex;
							if (settings_.enableReflectionCapture && (reflectionCubeDescIndex_ != 0) && (batchTransparent.materialHandle.id != 0))
							{
								const auto& mat = scene.GetMaterial(batchTransparent.materialHandle);
								if (mat.envSource == EnvSource::ReflectionCapture)
								{
									envDescIndex = reflectionCubeDescIndex_;
								}
							}
							ctx.commandList.BindTextureDesc(17, envDescIndex);

							std::uint32_t flags = 0;
							if (useTex)
							{
								flags |= kFlagUseTex;
							}
							if (useShadow)
							{
								flags |= kFlagUseShadow;
							}
							if (batchTransparent.material.normalDescIndex != 0)
							{
								flags |= kFlagUseNormal;
							}
							if (batchTransparent.material.metalnessDescIndex != 0)
							{
								flags |= kFlagUseMetalTex;
							}
							if (batchTransparent.material.roughnessDescIndex != 0)
							{
								flags |= kFlagUseRoughTex;
							}
							if (batchTransparent.material.aoDescIndex != 0)
							{
								flags |= kFlagUseAOTex;
							}
							if (batchTransparent.material.emissiveDescIndex != 0)
							{
								flags |= kFlagUseEmissiveTex;
							}
							if (envDescIndex != 0)
							{
								flags |= kFlagUseEnv;
								if (envDescIndex == scene.skyboxDescIndex)
								{
									flags |= kFlagEnvFlipZ;
								}
								// Dynamic reflection captures update only mip0. Force mip0 sampling in the shader to
								// avoid seams/garbage when the cube texture was created with a full mip chain.
								if (settings_.enableReflectionCapture && (reflectionCubeDescIndex_ != 0) && (envDescIndex == reflectionCubeDescIndex_))
								{
									flags |= kFlagEnvForceMip0;
								}
							}

							PerBatchConstants constants{};
							const mathUtils::Mat4 viewProjT = mathUtils::Transpose(viewProj);
							const mathUtils::Mat4 dirVP_T = mathUtils::Transpose(dirLightViewProj);

							std::memcpy(constants.uViewProj.data(), mathUtils::ValuePtr(viewProjT), sizeof(float) * 16);
							std::memcpy(constants.uLightViewProj.data(), mathUtils::ValuePtr(dirVP_T), sizeof(float) * 16);

							constants.uCameraAmbient = { camPosLocal.x, camPosLocal.y, camPosLocal.z, 0.22f };
							constants.uCameraForward = { camFLocal.x, camFLocal.y, camFLocal.z, 0.0f };
							constants.uBaseColor = { batchTransparent.material.baseColor.x, batchTransparent.material.baseColor.y, batchTransparent.material.baseColor.z, batchTransparent.material.baseColor.w };

							const float materialBiasTexels = batchTransparent.material.shadowBias;
							constants.uMaterialFlags = { 0.0f, 0.0f, materialBiasTexels, AsFloatBits(flags) };

							constants.uPbrParams = { batchTransparent.material.metallic, batchTransparent.material.roughness, batchTransparent.material.ao, batchTransparent.material.emissiveStrength };

							constants.uCounts = {
								static_cast<float>(lightCount),
								static_cast<float>(spotShadows.size()),
								static_cast<float>(pointShadows.size()),
								0.0f
							};

							constants.uShadowBias = {
								settings_.dirShadowBaseBiasTexels,
								settings_.spotShadowBaseBiasTexels,
								settings_.pointShadowBaseBiasTexels,
								settings_.shadowSlopeScaleTexels
							};

							ctx.commandList.BindInputLayout(batchTransparent.mesh->layoutInstanced);
							ctx.commandList.BindVertexBuffer(0, batchTransparent.mesh->vertexBuffer, batchTransparent.mesh->vertexStrideBytes, 0);
							ctx.commandList.BindVertexBuffer(1, instanceBuffer_, instStride, batchTransparent.instanceOffset * instStride);
							ctx.commandList.BindIndexBuffer(batchTransparent.mesh->indexBuffer, batchTransparent.mesh->indexType, 0);

							ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &constants, 1 }));

							// IMPORTANT: transparent = one object per draw (instanceCount = 1)
							ctx.commandList.DrawIndexed(batchTransparent.mesh->indexCount, batchTransparent.mesh->indexType, 0, 0, 1, 0);
						}
					}


					// ImGui overlay (optional)
					if (imguiDrawData)
					{
						ctx.commandList.DX12ImGuiRender(imguiDrawData);
					}
				});

