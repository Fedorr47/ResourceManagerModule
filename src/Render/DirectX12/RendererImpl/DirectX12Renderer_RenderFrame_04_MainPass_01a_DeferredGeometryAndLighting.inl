{
	const FrameCameraData frameCamera = BuildFrameCameraData(scene, scDesc.extent);
	const mathUtils::Mat4& proj = frameCamera.proj;
	const mathUtils::Mat4& view = frameCamera.view;
	const mathUtils::Mat4& viewProj = frameCamera.viewProj;
	const mathUtils::Mat4& invViewProj = frameCamera.invViewProj;
	const mathUtils::Mat4& invViewProjT = frameCamera.invViewProjT;
	const mathUtils::Vec3& camPosLocal = frameCamera.camPos;
	const mathUtils::Vec3& camFLocal = frameCamera.camForward;

	deferredReflectionProbesScratch_.clear();
	deferredReflectionProbeRemapScratch_.assign(reflectionProbes_.size(), -1);
	auto& deferredReflectionProbes = deferredReflectionProbesScratch_;
	auto& deferredReflectionProbeRemap = deferredReflectionProbeRemapScratch_;
	const float probeHalfExtent = settings_.reflectionProbeBoxHalfExtent;
	for (std::size_t probeIndex = 0; probeIndex < reflectionProbes_.size(); ++probeIndex)
	{
		const auto& probe = reflectionProbes_[probeIndex];
		if (!probe.cube || probe.cubeDescIndex == 0)
		{
			continue;
		}
		DeferredReflectionProbeGpu gpu{};
		gpu.boxMin = {
			probe.capturePos.x - probeHalfExtent,
			probe.capturePos.y - probeHalfExtent,
			probe.capturePos.z - probeHalfExtent,
			0.0f
		};
		gpu.boxMax = {
			probe.capturePos.x + probeHalfExtent,
			probe.capturePos.y + probeHalfExtent,
			probe.capturePos.z + probeHalfExtent,
			0.0f
		};
		gpu.capturePosDesc = {
			probe.capturePos.x,
			probe.capturePos.y,
			probe.capturePos.z,
			AsFloatBits(static_cast<std::uint32_t>(probe.cubeDescIndex))
		};
		deferredReflectionProbeRemap[probeIndex] =
			static_cast<int>(deferredReflectionProbes.size());
		deferredReflectionProbes.push_back(gpu);
	}

	activeReflectionProbeCount =
		std::min<std::uint32_t>(
			static_cast<std::uint32_t>(deferredReflectionProbes.size()),
			255u);
	if (activeReflectionProbeCount > 0u)
	{
		device_.UpdateBuffer(
			reflectionProbeMetaBuffer_,
			std::as_bytes(std::span{ deferredReflectionProbes }));
	}

	DeferredLightingConstants deferredConstants{};
	std::memcpy(deferredConstants.uInvViewProj.data(), mathUtils::ValuePtr(invViewProjT), sizeof(float) * 16);
	deferredConstants.uCameraPosAmbient = { camPosLocal.x, camPosLocal.y, camPosLocal.z, 0.22f };
	deferredConstants.uCameraForward = { camFLocal.x, camFLocal.y, camFLocal.z, 0.0f };
	deferredConstants.uShadowBias = {
		settings_.dirShadowBaseBiasTexels,
		settings_.spotShadowBaseBiasTexels,
		settings_.pointShadowBaseBiasTexels,
		settings_.shadowSlopeScaleTexels
	};
	deferredConstants.uCounts = {
	static_cast<float>(lightCount),
	static_cast<float>(spotShadows.size()),
	static_cast<float>(pointShadows.size()),
	static_cast<float>(activeReflectionProbeCount)
	};

	// Import swapchain depth as an external RenderGraph texture so offscreen passes can use it.
	const auto depthRG = graph.ImportTexture(
		swapChain.GetDepthTexture(),
		renderGraph::RGTextureDesc{
			.extent = scDesc.extent,
			.format = rhi::Format::D24_UNORM_S8_UINT,
			.usage = renderGraph::ResourceUsage::DepthStencil,
			.debugName = "SwapChainDepth_Imported"
		});

	const auto gbuf0 = graph.CreateTexture(renderGraph::RGTextureDesc{
		.extent = scDesc.extent,
		.format = rhi::Format::RGBA8_UNORM,
		.usage = renderGraph::ResourceUsage::RenderTarget,
		.debugName = "GBuffer0_AlbedoRough"
		});
	const auto gbuf1 = graph.CreateTexture(renderGraph::RGTextureDesc{
		.extent = scDesc.extent,
		.format = rhi::Format::RGBA8_UNORM,
		.usage = renderGraph::ResourceUsage::RenderTarget,
		.debugName = "GBuffer1_NormalMetal"
		});
	const auto gbuf2 = graph.CreateTexture(renderGraph::RGTextureDesc{
		.extent = scDesc.extent,
		.format = rhi::Format::RGBA8_UNORM,
		.usage = renderGraph::ResourceUsage::RenderTarget,
		.debugName = "GBuffer2_EmissiveAO"
		});
	const auto gbuf3 = graph.CreateTexture(renderGraph::RGTextureDesc{
		.extent = scDesc.extent,
		.format = rhi::Format::RGBA8_UNORM,
		.usage = renderGraph::ResourceUsage::RenderTarget,
		.debugName = "GBuffer3_EnvSel"
		});
	const auto sceneColor = graph.CreateTexture(renderGraph::RGTextureDesc{
		.extent = scDesc.extent,
		.format = rhi::Format::RGBA8_UNORM,
		.usage = renderGraph::ResourceUsage::RenderTarget,
		.debugName = "SceneColor_Lit"
		});

	auto sceneColorAfterFog = sceneColor;

	// --- GBuffer pass (opaque) ---
	{
		renderGraph::PassAttachments att{};
		att.useSwapChainBackbuffer = false;
		att.colors = { gbuf0, gbuf1, gbuf2, gbuf3 };
		att.depth = depthRG;

		att.clearDesc.clearColor = true;
		att.clearDesc.clearDepth = true;
		att.clearDesc.clearStencil = true;
		att.clearDesc.color = { 0.0f, 0.0f, 0.0f, 0.0f };
		att.clearDesc.depth = 1.0f;
		att.clearDesc.stencil = 0;

		graph.AddPass("GBufferPass", std::move(att),
			[this,
			&scene,
			dirLightViewProj,
			lightCount,
			mainBatches,
			instStride,
			gbuf0,
			gbuf1,
			gbuf2,
			gbuf3,
			activeReflectionProbeCount,
			selectionOpaque,
			selectionTransparent,
			selectionInstances,
			DrawEditorSelectionGroup,
			ComputeDeferredGBufferReflectionMeta,
			deferredReflectionProbeRemap](renderGraph::PassContext& ctx)
		{
			const auto extent = ctx.passExtent;

			ctx.commandList.SetViewport(0, 0,
				static_cast<int>(extent.width),
				static_cast<int>(extent.height));

			ctx.commandList.SetState(state_);
			ctx.commandList.BindPipeline(psoDeferredGBuffer_);

			const FrameCameraData camera = BuildFrameCameraData(scene, extent);
			const mathUtils::Mat4& viewProj = camera.viewProj;
			const mathUtils::Vec3& camPosLocal = camera.camPos;
			const mathUtils::Vec3& camFLocal = camera.camForward;

			// Flags must match shader.
			constexpr std::uint32_t kFlagUseTex = 1u << 0;
			constexpr std::uint32_t kFlagUseNormal = 1u << 2;
			constexpr std::uint32_t kFlagUseMetalTex = 1u << 3;
			constexpr std::uint32_t kFlagUseRoughTex = 1u << 4;
			constexpr std::uint32_t kFlagUseAOTex = 1u << 5;
			constexpr std::uint32_t kFlagUseEmissiveTex = 1u << 6;

			for (const Batch& batch : mainBatches)
			{
				if (!batch.mesh || batch.instanceCount == 0)
				{
					continue;
				}

				MaterialPerm perm = MaterialPerm::None;
				if (batch.materialHandle.id != 0)
				{
					perm = EffectivePerm(scene.GetMaterial(batch.materialHandle));
				}
				else
				{
					if (batch.material.albedoDescIndex != 0)
					{
						perm = perm | MaterialPerm::UseTex;
					}
				}

				std::uint32_t flags = 0u;
				if (HasFlag(perm, MaterialPerm::UseTex) && batch.material.albedoDescIndex != 0)
				{
					flags |= kFlagUseTex;
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

				const auto [envSourceForGBuffer, probeIdxNForGBuffer] = ComputeDeferredGBufferReflectionMeta(
					batch.materialHandle,
					batch.reflectionProbeIndex,
					deferredReflectionProbeRemap,
					activeReflectionProbeCount);

				// Material textures via descriptors (same slots as forward).
				ctx.commandList.BindTextureDesc(0, batch.material.albedoDescIndex);
				ctx.commandList.BindTextureDesc(12, batch.material.normalDescIndex);
				ctx.commandList.BindTextureDesc(13, batch.material.metalnessDescIndex);
				ctx.commandList.BindTextureDesc(14, batch.material.roughnessDescIndex);
				ctx.commandList.BindTextureDesc(15, batch.material.aoDescIndex);
				ctx.commandList.BindTextureDesc(16, batch.material.emissiveDescIndex);

				PerBatchConstants constants{};
				const mathUtils::Mat4 viewProjT = mathUtils::Transpose(viewProj);
				const mathUtils::Mat4 dirVP_T = mathUtils::Transpose(dirLightViewProj);
				std::memcpy(constants.uViewProj.data(), mathUtils::ValuePtr(viewProjT), sizeof(float) * 16);
				std::memcpy(constants.uLightViewProj.data(), mathUtils::ValuePtr(dirVP_T), sizeof(float) * 16);

				constants.uCameraAmbient = { camPosLocal.x, camPosLocal.y, camPosLocal.z, 0.0f };
				constants.uCameraForward = { camFLocal.x, camFLocal.y, camFLocal.z, 0.0f };
				constants.uBaseColor = { batch.material.baseColor.x, batch.material.baseColor.y, batch.material.baseColor.z, batch.material.baseColor.w };

				constants.uMaterialFlags = { 0.0f, 0.0f, 0.0f, AsFloatBits(flags) };
				constants.uPbrParams = { batch.material.metallic, batch.material.roughness, batch.material.ao, batch.material.emissiveStrength };
				constants.uCounts = {
					static_cast<float>(lightCount),
					0.0f,
					0.0f,
					static_cast<float>(activeReflectionProbeCount)
				};
				constants.uShadowBias = { 0.0f, 0.0f, 0.0f, 0.0f };
				constants.uEnvProbeBoxMin = { 0.0f, 0.0f, 0.0f, envSourceForGBuffer };
				constants.uEnvProbeBoxMax = { 0.0f, 0.0f, 0.0f, probeIdxNForGBuffer };


				// Bindless indices for DeferredGBuffer_dx12.hlsl (space1 SRV heap).
				constants.uTexIndices0 = {
					static_cast<float>(batch.material.albedoDescIndex),
					static_cast<float>(batch.material.normalDescIndex),
					static_cast<float>(batch.material.metalnessDescIndex),
					static_cast<float>(batch.material.roughnessDescIndex)
				};
				constants.uTexIndices1 = {
					static_cast<float>(batch.material.aoDescIndex),
					static_cast<float>(batch.material.emissiveDescIndex),
					0.0f,
					0.0f
				};

				// IA (instanced)
				ctx.commandList.BindInputLayout(batch.mesh->layoutInstanced);
				ctx.commandList.BindVertexBuffer(0, batch.mesh->vertexBuffer, batch.mesh->vertexStrideBytes, 0);
				ctx.commandList.BindVertexBuffer(1, instanceBuffer_, instStride, batch.instanceOffset * instStride);
				ctx.commandList.BindIndexBuffer(batch.mesh->indexBuffer, batch.mesh->indexType, 0);

				ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &constants, 1 }));
				ctx.commandList.DrawIndexed(batch.mesh->indexCount, batch.mesh->indexType, 0, 0, batch.instanceCount, 0);
			}
		});
	}

	// --- SSAO (v1): fullscreen AO from depth+normal, then depth-aware blur ---
	const auto ssaoRaw = graph.CreateTexture(renderGraph::RGTextureDesc{
		.extent = scDesc.extent,
		.format = rhi::Format::R32_FLOAT,
		.usage = renderGraph::ResourceUsage::RenderTarget,
		.debugName = "SSAO_Raw"
		});
	const auto ssaoBlur = graph.CreateTexture(renderGraph::RGTextureDesc{
		.extent = scDesc.extent,
		.format = rhi::Format::R32_FLOAT,
		.usage = renderGraph::ResourceUsage::RenderTarget,
		.debugName = "SSAO_Blur"
		});


	// SSAO
	{
		renderGraph::PassAttachments att{};
		att.useSwapChainBackbuffer = false;
		att.colors = { ssaoRaw };
		att.clearDesc.clearColor = true;
		att.clearDesc.clearDepth = false;
		att.clearDesc.clearStencil = false;
		att.clearDesc.color = { 1.0f, 1.0f, 1.0f, 1.0f };

		graph.AddPass("SSAO", std::move(att),
			[this, &scene, depthRG, gbuf1, ssaoRaw](renderGraph::PassContext& ctx)
			{
				const auto extent = ctx.passExtent;
				ctx.commandList.SetViewport(0, 0,
					static_cast<int>(extent.width),
					static_cast<int>(extent.height));

				const FrameCameraData camera = BuildFrameCameraData(scene, extent);
				const mathUtils::Mat4& invViewProjT = camera.invViewProjT;

				SSAOConstants c{};
				std::memcpy(c.uInvViewProj.data(), mathUtils::ValuePtr(invViewProjT), sizeof(float) * 16);
				// Tunables (v1)
				c.uParams = {
					settings_.enableSSAO ? settings_.ssaoRadius : 0.0f,
					settings_.ssaoBias, settings_.ssaoStrength, settings_.ssaoPower
				};
				c.uInvSize = {
					extent.width ? (1.0f / static_cast<float>(extent.width)) : 0.0f,
					extent.height ? (1.0f / static_cast<float>(extent.height)) : 0.0f,
					0.0f,
					0.0f
				};

				ctx.commandList.SetState(deferredLightingState_);
				ctx.commandList.BindPipeline(psoSSAO_);
				ctx.commandList.BindInputLayout(fullscreenLayout_);
				ctx.commandList.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);

				// t0 = gbuf1 normals, t1 = depth
				ctx.commandList.BindTexture2D(0, ctx.resources.GetTexture(gbuf1));
				ctx.commandList.BindTexture2D(1, ctx.resources.GetTexture(depthRG));
				ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &c, 1 }));
				ctx.commandList.Draw(3);
			});
	}

	// SSAO blur
	{
		renderGraph::PassAttachments att{};
		att.useSwapChainBackbuffer = false;
		att.colors = { ssaoBlur };
		att.clearDesc.clearColor = true;
		att.clearDesc.clearDepth = false;
		att.clearDesc.clearStencil = false;
		att.clearDesc.color = { 1.0f, 1.0f, 1.0f, 1.0f };

		graph.AddPass("SSAOBlur", std::move(att),
			[this, depthRG, ssaoRaw, ssaoBlur](renderGraph::PassContext& ctx)
			{
				const auto extent = ctx.passExtent;
				ctx.commandList.SetViewport(0, 0,
					static_cast<int>(extent.width),
					static_cast<int>(extent.height));

				SSAOBlurConstants c{};
				c.uInvSize = {
					extent.width ? (1.0f / static_cast<float>(extent.width)) : 0.0f,
					extent.height ? (1.0f / static_cast<float>(extent.height)) : 0.0f,
					0.0f,
					0.0f
				};
				c.uParams = { settings_.ssaoBlurDepthThreshold, 0.0f, 0.0f, 0.0f };

				ctx.commandList.SetState(deferredLightingState_);
				ctx.commandList.BindPipeline(psoSSAOBlur_);
				ctx.commandList.BindInputLayout(fullscreenLayout_);
				ctx.commandList.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);

				// t0 = ssao raw, t1 = depth
				ctx.commandList.BindTexture2D(0, ctx.resources.GetTexture(ssaoRaw));
				ctx.commandList.BindTexture2D(1, ctx.resources.GetTexture(depthRG));
				ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &c, 1 }));
				ctx.commandList.Draw(3);
			});
	}

	// --- Fullscreen deferred lighting into SceneColor (do NOT bind depth as DS so it can be sampled as SRV) ---
	{
		renderGraph::PassAttachments att{};
		att.useSwapChainBackbuffer = false;
		att.colors = { sceneColorAfterFog };

		att.clearDesc.clearColor = true;
		att.clearDesc.clearDepth = false;
		att.clearDesc.clearStencil = false;
		att.clearDesc.color = { 0.0f, 0.0f, 0.0f, 1.0f };

		graph.AddPass("DeferredLighting", std::move(att),
			[this, &scene, gbuf0, gbuf1, gbuf2, gbuf3, depthRG, shadowRG, spotShadows, pointShadows, deferredConstants, ssaoBlur, activeReflectionProbeCount](renderGraph::PassContext& ctx)
			{
				const auto extent = ctx.passExtent;

				ctx.commandList.SetViewport(0, 0,
					static_cast<int>(extent.width),
					static_cast<int>(extent.height));

				ctx.commandList.SetState(deferredLightingState_);
				ctx.commandList.BindPipeline(psoDeferredLighting_);
				ctx.commandList.BindInputLayout(fullscreenLayout_);
				ctx.commandList.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);

				// Registers must match DeferredLighting_dx12.hlsl
				ctx.commandList.BindTexture2D(0, ctx.resources.GetTexture(gbuf0)); // t0
				ctx.commandList.BindTexture2D(1, ctx.resources.GetTexture(gbuf1)); // t1
				ctx.commandList.BindTexture2D(2, ctx.resources.GetTexture(gbuf2)); // t2
				ctx.commandList.BindTexture2D(3, ctx.resources.GetTexture(depthRG)); // t3
				ctx.commandList.BindTexture2D(4, ctx.resources.GetTexture(gbuf3)); // t4 env selector
				ctx.commandList.BindTexture2D(5, ctx.resources.GetTexture(shadowRG)); // t5 dir CSM
				ctx.commandList.BindStructuredBufferSRV(6, shadowDataBuffer_); // t6 shadow metadata

				// Spot shadow maps (t7..t10)
				for (std::size_t spotShadowIndex = 0; spotShadowIndex < spotShadows.size(); ++spotShadowIndex)
				{
					const auto tex = ctx.resources.GetTexture(spotShadows[spotShadowIndex].tex);
					ctx.commandList.BindTexture2D(7 + static_cast<std::uint32_t>(spotShadowIndex), tex);
				}
				// Point shadow cubemaps (t11..t14)
				for (std::size_t pointShadowIndex = 0; pointShadowIndex < pointShadows.size(); ++pointShadowIndex)
				{
					const auto tex = ctx.resources.GetTexture(pointShadows[pointShadowIndex].cube);
					ctx.commandList.BindTexture2DArray(11 + static_cast<std::uint32_t>(pointShadowIndex), tex);
				}

				// Env cubemaps for IBL (t15 skybox, t17 reflection capture)
				ctx.commandList.BindTextureDesc(15, scene.skyboxDescIndex);

				// Lights (t16) and SSAO (t18); t19 = full reflection cube-array
				ctx.commandList.BindStructuredBufferSRV(16, lightsBuffer_);
				ctx.commandList.BindTexture2D(18, ctx.resources.GetTexture(ssaoBlur));

				if (activeReflectionProbeCount > 0u)
				{
					ctx.commandList.BindStructuredBufferSRV(19, reflectionProbeMetaBuffer_);
				}

				ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &deferredConstants, 1 }));
				ctx.commandList.Draw(3);
			});
	}