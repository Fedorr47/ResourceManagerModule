// NOTE: This path is for deferred rendering with offscreen SceneColor.
// It composites reflections into `sceneColor` before final Present to swapchain.
// We render a per-mirror screen-space mask into an offscreen RT, render the reflected scene
// into an offscreen color+depth, then alpha-blend it into the swapchain using the mask.

if (settings_.enablePlanarReflections && !planarMirrorDraws.empty())
{
	const auto extent = scDesc.extent;
	const std::uint32_t maxMirrors = std::max(1u, settings_.planarReflectionMaxMirrors);

	std::uint32_t mirrorIndex = 0u;
	for (const PlanarMirrorDraw& mirror : planarMirrorDraws)
	{
		if (!mirror.mesh || mirror.mesh->indexCount == 0)
		{
			continue;
		}
		if (mirrorIndex >= maxMirrors)
		{
			break;
		}

		auto [planeN, planeD] = CanonicalizePlane(mirror.planeNormal, mirror.planePoint);
		{
			const mathUtils::Vec3 camPosLocal = scene.camera.position;
			if (mathUtils::Dot(planeN, camPosLocal) + planeD < 0.0f)
			{
				planeN = -planeN;
				planeD = -planeD;
			}
		}

		const auto maskTex = graph.CreateTexture(renderGraph::RGTextureDesc{
			.extent = extent,
			.format = rhi::Format::RGBA8_UNORM,
			.usage = renderGraph::ResourceUsage::RenderTarget,
			.debugName = std::string("PlanarMask_") + std::to_string(mirrorIndex)
			});

		const auto reflColor = graph.CreateTexture(renderGraph::RGTextureDesc{
			.extent = extent,
			.format = rhi::Format::RGBA8_UNORM,
			.usage = renderGraph::ResourceUsage::RenderTarget,
			.debugName = std::string("PlanarReflColor_") + std::to_string(mirrorIndex)
			});

		const auto reflDepth = graph.CreateTexture(renderGraph::RGTextureDesc{
			.extent = extent,
			.format = rhi::Format::D24_UNORM_S8_UINT,
			.usage = renderGraph::ResourceUsage::DepthStencil,
			.debugName = std::string("PlanarReflDepth_") + std::to_string(mirrorIndex)
			});

		// (1) Mask: draw the mirror surface into mask alpha, depth-tested against main depth.
		{
			renderGraph::PassAttachments att{};
			att.useSwapChainBackbuffer = false;
			att.colors = { maskTex };
			att.depth = depthRG;
			att.clearDesc.clearColor = true;
			att.clearDesc.color = { 0.0f, 0.0f, 0.0f, 0.0f };
			att.clearDesc.clearDepth = false;
			att.clearDesc.clearStencil = false;

			graph.AddPass(std::string("PlanarMask_") + std::to_string(mirrorIndex), std::move(att),
				[this, &scene, mirror, instStride](renderGraph::PassContext& ctx)
				{
					const auto e = ctx.passExtent;
					ctx.commandList.SetViewport(0, 0, static_cast<int>(e.width), static_cast<int>(e.height));

					ctx.commandList.SetState(planarMaskState_);
					ctx.commandList.SetStencilRef(0x01u);

					ctx.commandList.BindPipeline(psoHighlight_);

					const float aspect = e.height ? (static_cast<float>(e.width) / static_cast<float>(e.height)) : 1.0f;
					const mathUtils::Mat4 proj = mathUtils::PerspectiveRH_ZO(mathUtils::DegToRad(scene.camera.fovYDeg), aspect, scene.camera.nearZ, scene.camera.farZ);
					const mathUtils::Mat4 view = mathUtils::LookAt(scene.camera.position, scene.camera.target, scene.camera.up);
					const mathUtils::Mat4 viewProjT = mathUtils::Transpose(proj * view);

					PerBatchConstants constants{};
					std::memcpy(constants.uViewProj.data(), mathUtils::ValuePtr(viewProjT), sizeof(float) * 16);
					std::memcpy(constants.uLightViewProj.data(), mathUtils::ValuePtr(viewProjT), sizeof(float) * 16);
					constants.uCameraAmbient = { scene.camera.position.x, scene.camera.position.y, scene.camera.position.z, 0.0f };
					constants.uCameraForward = { 0.0f, 0.0f, 0.0f, 0.0f };
					// CORE_HIGHLIGHT path uses uBaseColor; write mask into alpha.
					constants.uBaseColor = { 0.0f, 0.0f, 0.0f, 1.0f };
					constants.uMaterialFlags = { 0.0f, 0.0f, 0.0f, AsFloatBits(0u) };
					constants.uPbrParams = { 0.0f, 0.0f, 0.0f, 0.0f };
					constants.uCounts = { 0.0f, 0.0f, 0.0f, 0.0f };
					constants.uShadowBias = { 0.0f, 0.0f, 0.0f, 0.0f };
					constants.uEnvProbeBoxMin = { 0.0f, 0.0f, 0.0f, 0.0f };
					constants.uEnvProbeBoxMax = { 0.0f, 0.0f, 0.0f, 0.0f };

					ctx.commandList.BindInputLayout(mirror.mesh->layoutInstanced);
					ctx.commandList.BindVertexBuffer(0, mirror.mesh->vertexBuffer, mirror.mesh->vertexStrideBytes, 0);
					ctx.commandList.BindVertexBuffer(1, instanceBuffer_, instStride, mirror.instanceOffset * instStride);
					ctx.commandList.BindIndexBuffer(mirror.mesh->indexBuffer, mirror.mesh->indexType, 0);
					ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &constants, 1 }));
					ctx.commandList.DrawIndexed(mirror.mesh->indexCount, mirror.mesh->indexType, 0, 0, 1, 0);
				});
		}

		// (2) Reflected scene: render mirrored scene into reflColor/reflDepth.
		{
			renderGraph::PassAttachments att{};
			att.useSwapChainBackbuffer = false;
			att.colors = { reflColor };
			att.depth = reflDepth;
			att.clearDesc.clearColor = true;
			att.clearDesc.color = { 0.0f, 0.0f, 0.0f, 0.0f };
			att.clearDesc.clearDepth = true;
			att.clearDesc.depth = 1.0f;
			att.clearDesc.clearStencil = true;
			att.clearDesc.stencil = 0;

			graph.AddPass(std::string("PlanarReflScene_") + std::to_string(mirrorIndex), std::move(att),
				[this,
				&scene, 
				shadowRG, 
				dirLightViewProj, 
				lightCount, 
				spotShadows, 
				pointShadows, 
				mainBatches, 
				captureMainBatchesNoCull, 
				instStride, 
				planeN, 
				planeD,
				ResolveMainPassMaterialPerm,
				ResolveOpaqueEnvBinding,
				BindMainPassMaterialTextures,
				BuildMainPassMaterialFlags
				](renderGraph::PassContext& ctx)
				{
					const auto e = ctx.passExtent;
					ctx.commandList.SetViewport(0, 0, static_cast<int>(e.width), static_cast<int>(e.height));

					const float aspect = e.height ? (static_cast<float>(e.width) / static_cast<float>(e.height)) : 1.0f;
					const mathUtils::Mat4 proj = mathUtils::PerspectiveRH_ZO(mathUtils::DegToRad(scene.camera.fovYDeg), aspect, scene.camera.nearZ, scene.camera.farZ);
					const mathUtils::Mat4 view = mathUtils::LookAt(scene.camera.position, scene.camera.target, scene.camera.up);
					const mathUtils::Mat4 viewProj = proj * view;
					const mathUtils::Vec3 camPosLocal = scene.camera.position;
					const mathUtils::Vec3 camFLocal = mathUtils::Normalize(scene.camera.target - scene.camera.position);

					const mathUtils::Mat4 reflectW = mathUtils::MakeReflectionMatrix(planeN, planeD);
					const mathUtils::Mat4 viewProjReflT = mathUtils::Transpose(viewProj * reflectW);

					ctx.commandList.SetStencilRef(0u);
					ctx.commandList.SetState(planarReflectedState_);

					// --- Skybox in planar reflection: draw into reflColor (background) ---
	// reflDepth was cleared to 1.0f, so regular skybox depth-test works here.
					if (scene.skyboxDescIndex != 0)
					{
						const mathUtils::Vec3 camPosRefl = ReflectPoint(camPosLocal, planeN, planeD);
						const mathUtils::Vec3 camFwdRefl = ReflectVector(camFLocal, planeN);
						const mathUtils::Vec3 camUpRefl = ReflectVector(scene.camera.up, planeN);

						const mathUtils::Mat4 viewRefl = mathUtils::LookAt(
							camPosRefl,
							camPosRefl + camFwdRefl,
							camUpRefl);

						mathUtils::Mat4 viewReflNoTranslation = viewRefl;
						viewReflNoTranslation[3] = mathUtils::Vec4(0.0f, 0.0f, 0.0f, 1.0f);

						const mathUtils::Mat4 viewProjSkyReflT =
							mathUtils::Transpose(proj * viewReflNoTranslation);

						SkyboxConstants sky{};
						std::memcpy(sky.uViewProj.data(), mathUtils::ValuePtr(viewProjSkyReflT), sizeof(float) * 16);

						rhi::GraphicsState skyState = skyboxState_;
						// No stencil needed here: reflColor will be masked later by maskTex in PlanarComposite.
						skyState.depth.stencil.enable = false;

						ctx.commandList.SetState(skyState);
						ctx.commandList.BindPipeline(psoSkybox_);
						ctx.commandList.BindTextureDesc(0, scene.skyboxDescIndex);
						ctx.commandList.BindInputLayout(skyboxMesh_.layout);
						ctx.commandList.BindVertexBuffer(0, skyboxMesh_.vertexBuffer, skyboxMesh_.vertexStrideBytes, 0);
						ctx.commandList.BindIndexBuffer(skyboxMesh_.indexBuffer, skyboxMesh_.indexType, 0);
						ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &sky, 1 }));
						ctx.commandList.DrawIndexed(skyboxMesh_.indexCount, skyboxMesh_.indexType, 0, 0, 1, 0);

						// Restore for reflected meshes
						ctx.commandList.SetState(planarReflectedState_);
					}

					// Bind shadows + lights like in the forward main pass.
					ctx.commandList.BindTexture2D(1, ctx.resources.GetTexture(shadowRG));
					for (std::size_t spotShadowIndex = 0; spotShadowIndex < spotShadows.size(); ++spotShadowIndex)
					{
						ctx.commandList.BindTexture2D(3 + static_cast<std::uint32_t>(spotShadowIndex), ctx.resources.GetTexture(spotShadows[spotShadowIndex].tex));
					}
					for (std::size_t pointShadowIndex = 0; pointShadowIndex < pointShadows.size(); ++pointShadowIndex)
					{
						ctx.commandList.BindTexture2DArray(7 + static_cast<std::uint32_t>(pointShadowIndex), ctx.resources.GetTexture(pointShadows[pointShadowIndex].cube));
					}
					ctx.commandList.BindStructuredBufferSRV(11, shadowDataBuffer_);
					ctx.commandList.BindStructuredBufferSRV(2, lightsBuffer_);

					const auto& planarBatches = !captureMainBatchesNoCull.empty() ? captureMainBatchesNoCull : mainBatches;

					for (const Batch& batch : planarBatches)
					{
						if (!batch.mesh || batch.instanceCount == 0)
						{
							continue;
						}

						MaterialPerm perm = ResolveMainPassMaterialPerm(batch.material, batch.materialHandle);

						if (HasFlag(perm, MaterialPerm::PlanarMirror))
						{
							continue;
						}

						const bool useTex = HasFlag(perm, MaterialPerm::UseTex);
						const bool useShadow = HasFlag(perm, MaterialPerm::UseShadow);

						ctx.commandList.BindPipeline(PlanarPipelineFor(perm));
						const ResolvedMaterialEnvBinding env = ResolveOpaqueEnvBinding(batch.materialHandle, batch.reflectionProbeIndex);
						BindMainPassMaterialTextures(ctx.commandList, batch.material, env);

						const std::uint32_t flags = BuildMainPassMaterialFlags(batch.material, useTex, useShadow, env);

						PerBatchConstants constants{};
						const mathUtils::Mat4 dirVP_T = mathUtils::Transpose(dirLightViewProj);
						std::memcpy(constants.uViewProj.data(), mathUtils::ValuePtr(viewProjReflT), sizeof(float) * 16);
						std::memcpy(constants.uLightViewProj.data(), mathUtils::ValuePtr(dirVP_T), sizeof(float) * 16);

						const mathUtils::Vec3 camPosRefl = ReflectPoint(camPosLocal, planeN, planeD);
						constants.uCameraAmbient = { camPosRefl.x, camPosRefl.y, camPosRefl.z, 0.22f };
						const mathUtils::Vec3 camFwdRefl = ReflectVector(camFLocal, planeN);
						constants.uCameraForward = { camFwdRefl.x, camFwdRefl.y, camFwdRefl.z, planeN.z };
						constants.uBaseColor = { batch.material.baseColor.x, batch.material.baseColor.y, batch.material.baseColor.z, batch.material.baseColor.w };

						const float materialBiasTexels = batch.material.shadowBias;
						constants.uMaterialFlags = { planeN.x, planeN.y, materialBiasTexels, AsFloatBits(flags) };

						constants.uPbrParams = { batch.material.metallic, batch.material.roughness, batch.material.ao, batch.material.emissiveStrength };
						constants.uCounts = { float(lightCount), float(spotShadows.size()), float(pointShadows.size()), (planeD - 0.05f) };
						constants.uShadowBias = { settings_.dirShadowBaseBiasTexels, settings_.spotShadowBaseBiasTexels, settings_.pointShadowBaseBiasTexels, settings_.shadowSlopeScaleTexels };
						constants.uEnvProbeBoxMin = { 0.0f, 0.0f, 0.0f, 0.0f };
						constants.uEnvProbeBoxMax = { 0.0f, 0.0f, 0.0f, 0.0f };

						if (env.usingReflectionProbeEnv && batch.reflectionProbeIndex >= 0 && static_cast<std::size_t>(batch.reflectionProbeIndex) < reflectionProbes_.size())
						{
							const auto& probe = reflectionProbes_[static_cast<std::size_t>(batch.reflectionProbeIndex)];
							const float h = settings_.reflectionProbeBoxHalfExtent;
							constants.uEnvProbeBoxMin = { probe.capturePos.x - h, probe.capturePos.y - h, probe.capturePos.z - h, 0.0f };
							constants.uEnvProbeBoxMax = { probe.capturePos.x + h, probe.capturePos.y + h, probe.capturePos.z + h, 0.0f };
						}

						ctx.commandList.BindInputLayout(batch.mesh->layoutInstanced);
						ctx.commandList.BindVertexBuffer(0, batch.mesh->vertexBuffer, batch.mesh->vertexStrideBytes, 0);
						ctx.commandList.BindVertexBuffer(1, instanceBuffer_, instStride, batch.instanceOffset * instStride);
						ctx.commandList.BindIndexBuffer(batch.mesh->indexBuffer, batch.mesh->indexType, 0);
						ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &constants, 1 }));
						ctx.commandList.DrawIndexed(batch.mesh->indexCount, batch.mesh->indexType, 0, 0, batch.instanceCount, 0);
					}
				});
		}

		// (3) Composite into SceneColor (alpha blend by mask).
		{
			renderGraph::PassAttachments att{};
			att.useSwapChainBackbuffer = false;
			att.colors = { sceneColor };
			att.depth = depthRG;
			att.clearDesc.clearColor = false;
			att.clearDesc.clearDepth = false;
			att.clearDesc.clearStencil = false;

			graph.AddPass(std::string("PlanarComposite_") + std::to_string(mirrorIndex), std::move(att),
				[this, &scene, view, proj, maskTex, reflColor, planeN, planeD](renderGraph::PassContext& ctx)
				{
					const auto e = ctx.passExtent;
					ctx.commandList.SetViewport(0, 0, static_cast<int>(e.width), static_cast<int>(e.height));
					ctx.commandList.SetStencilRef(0x01u);

					ctx.commandList.SetState(planarCompositeState_);
					ctx.commandList.BindPipeline(psoPlanarComposite_);
					ctx.commandList.BindInputLayout(fullscreenLayout_);
					ctx.commandList.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
					ctx.commandList.BindTexture2D(0, ctx.resources.GetTexture(maskTex));
					ctx.commandList.BindTexture2D(1, ctx.resources.GetTexture(reflColor));
					ctx.commandList.Draw(3);
				});
		}

		// (3b) Clear the planar stencil bit so later stencil users (outline, etc.) see a clean buffer.
		{
			renderGraph::PassAttachments att{};
			att.useSwapChainBackbuffer = false;
			att.colors = { maskTex }; // dummy RT (we only care about stencil writes)
			att.depth = depthRG;
			att.clearDesc.clearColor = false;
			att.clearDesc.clearDepth = false;
			att.clearDesc.clearStencil = false;

			graph.AddPass(std::string("PlanarStencilClear_") + std::to_string(mirrorIndex), std::move(att),
				[this, &scene, mirror, instStride](renderGraph::PassContext& ctx)
				{
					const auto e = ctx.passExtent;
					ctx.commandList.SetViewport(0, 0, static_cast<int>(e.width), static_cast<int>(e.height));
					ctx.commandList.SetState(planarMaskState_);
					ctx.commandList.SetStencilRef(0u);

					ctx.commandList.BindPipeline(psoHighlight_);

					const float aspect = e.height ? (static_cast<float>(e.width) / static_cast<float>(e.height)) : 1.0f;
					const mathUtils::Mat4 proj = mathUtils::PerspectiveRH_ZO(mathUtils::DegToRad(scene.camera.fovYDeg), aspect, scene.camera.nearZ, scene.camera.farZ);
					const mathUtils::Mat4 view = mathUtils::LookAt(scene.camera.position, scene.camera.target, scene.camera.up);
					const mathUtils::Mat4 viewProjT = mathUtils::Transpose(proj * view);

					PerBatchConstants constants{};
					std::memcpy(constants.uViewProj.data(), mathUtils::ValuePtr(viewProjT), sizeof(float) * 16);
					std::memcpy(constants.uLightViewProj.data(), mathUtils::ValuePtr(viewProjT), sizeof(float) * 16);
					constants.uCameraAmbient = { scene.camera.position.x, scene.camera.position.y, scene.camera.position.z, 0.0f };
					constants.uCameraForward = { 0.0f, 0.0f, 0.0f, 0.0f };
					constants.uBaseColor = { 0.0f, 0.0f, 0.0f, 0.0f };
					constants.uMaterialFlags = { 0.0f, 0.0f, 0.0f, AsFloatBits(0u) };
					constants.uPbrParams = { 0.0f, 0.0f, 0.0f, 0.0f };
					constants.uCounts = { 0.0f, 0.0f, 0.0f, 0.0f };
					constants.uShadowBias = { 0.0f, 0.0f, 0.0f, 0.0f };
					constants.uEnvProbeBoxMin = { 0.0f, 0.0f, 0.0f, 0.0f };
					constants.uEnvProbeBoxMax = { 0.0f, 0.0f, 0.0f, 0.0f };

					ctx.commandList.BindInputLayout(mirror.mesh->layoutInstanced);
					ctx.commandList.BindVertexBuffer(0, mirror.mesh->vertexBuffer, mirror.mesh->vertexStrideBytes, 0);
					ctx.commandList.BindVertexBuffer(1, instanceBuffer_, instStride, mirror.instanceOffset * instStride);
					ctx.commandList.BindIndexBuffer(mirror.mesh->indexBuffer, mirror.mesh->indexType, 0);
					ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &constants, 1 }));
					ctx.commandList.DrawIndexed(mirror.mesh->indexCount, mirror.mesh->indexType, 0, 0, 1, 0);
				});
		}

		++mirrorIndex;
	}
}