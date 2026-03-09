// --- Editor selection (opaque) over deferred SceneColor ---
if (!selectionOpaque.empty())
{
	renderGraph::PassAttachments att{};
	att.useSwapChainBackbuffer = false;
	att.colors = { sceneColorAfterFog };
	att.depth = depthRG;
	att.clearDesc.clearColor = false;
	att.clearDesc.clearDepth = false;
	att.clearDesc.clearStencil = false;

	graph.AddPass("DeferredSelectionOpaque", std::move(att),
		[this, &scene,
		sceneColor,
		depthRG,
		dirLightViewProj,
		selectionOpaque,
		selectionOpaqueStart,
		selectionInstances,
		DrawEditorSelectionGroup,
		instStride](renderGraph::PassContext& ctx)
		{
			const auto extent = ctx.passExtent;
			ctx.commandList.SetViewport(0, 0,
				static_cast<int>(extent.width),
				static_cast<int>(extent.height));

			const FrameCameraData camera = BuildFrameCameraData(scene, extent);
			const mathUtils::Mat4& viewProj = camera.viewProj;
			const mathUtils::Vec3& camPosLocal = camera.camPos;
			const mathUtils::Vec3& camFLocal = camera.camForward;

			DrawEditorSelectionGroup(
				ctx,
				state_,
				mathUtils::Vec4(1.0f, 0.86f, 0.10f, 0.22f),
				viewProj,
				dirLightViewProj,
				camPosLocal,
				camFLocal,
				extent,
				selectionOpaque,
				selectionOpaqueStart);
		});
}
// --- Transparent forward pass over deferred SceneColor ---
if (!transparentDraws.empty())
{
	renderGraph::PassAttachments att{};
	att.useSwapChainBackbuffer = false;
	att.colors = { sceneColorAfterFog };
	att.depth = depthRG;

	att.clearDesc.clearColor = false;
	att.clearDesc.clearDepth = false;
	att.clearDesc.clearStencil = false;

	graph.AddPass("DeferredTransparent", std::move(att),
		[this, &scene,
		shadowRG,
		dirLightViewProj,
		lightCount,
		spotShadows,
		pointShadows,
		transparentDraws,
		activeReflectionProbeCount,
		BindMainPassMaterialTextures,
		ResolveTransparentEnvBinding,
		ResolveMainPassMaterialPerm,
		BuildMainPassMaterialFlags,
		FillPerBatchViewLightingConstants,
		ResetPerBatchEnvProbeBox,
		instStride](renderGraph::PassContext& ctx)
	{
		const auto extent = ctx.passExtent;

		ctx.commandList.SetViewport(0, 0,
			static_cast<int>(extent.width),
			static_cast<int>(extent.height));

		ctx.commandList.SetState(transparentState_);

		const float aspect = extent.height
			? (static_cast<float>(extent.width) / static_cast<float>(extent.height))
			: 1.0f;

		const mathUtils::Mat4 proj = mathUtils::PerspectiveRH_ZO(mathUtils::DegToRad(scene.camera.fovYDeg), aspect, scene.camera.nearZ, scene.camera.farZ);
		const mathUtils::Mat4 view = mathUtils::LookAt(scene.camera.position, scene.camera.target, scene.camera.up);
		const mathUtils::Mat4 viewProj = proj * view;

		const mathUtils::Vec3 camPosLocal = scene.camera.position;
		const mathUtils::Vec3 camFLocal = mathUtils::Normalize(scene.camera.target - scene.camera.position);

		// Bind dir shadow map at t1.
		{
			const auto shadowTex = ctx.resources.GetTexture(shadowRG);
			if (shadowTex)
			{
				ctx.commandList.BindTexture2D(1, shadowTex);
			}
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

		for (const TransparentDraw& batchTransparent : transparentDraws)
		{
			if (!batchTransparent.mesh)
			{
				continue;
			}

			const MaterialPerm perm = ResolveMainPassMaterialPerm(
				batchTransparent.material,
				batchTransparent.materialHandle);
			const bool useTex = HasFlag(perm, MaterialPerm::UseTex);
			const bool useShadow = HasFlag(perm, MaterialPerm::UseShadow);
			const ResolvedMaterialEnvBinding env = ResolveTransparentEnvBinding(batchTransparent.materialHandle);

			ctx.commandList.BindPipeline(MainPipelineFor(perm));
			BindMainPassMaterialTextures(ctx.commandList, batchTransparent.material, env);

			const std::uint32_t flags = BuildMainPassMaterialFlags(
				batchTransparent.material,
				useTex,
				useShadow,
				env);

			PerBatchConstants constants{};
			FillPerBatchViewLightingConstants(constants, viewProj, dirLightViewProj, camPosLocal, camFLocal);
			constants.uBaseColor = { batchTransparent.material.baseColor.x, batchTransparent.material.baseColor.y, batchTransparent.material.baseColor.z, batchTransparent.material.baseColor.w };

			const float materialBiasTexels = batchTransparent.material.shadowBias;
			constants.uMaterialFlags = { 0.0f, 0.0f, materialBiasTexels, AsFloatBits(flags) };

			constants.uPbrParams = { batchTransparent.material.metallic, batchTransparent.material.roughness, batchTransparent.material.ao, batchTransparent.material.emissiveStrength };

			constants.uCounts = {
				static_cast<float>(lightCount),
				static_cast<float>(spotShadows.size()),
				static_cast<float>(pointShadows.size()),
				static_cast<float>(activeReflectionProbeCount)
			};

			constants.uShadowBias = {
				settings_.dirShadowBaseBiasTexels,
				settings_.spotShadowBaseBiasTexels,
				settings_.pointShadowBaseBiasTexels,
				settings_.shadowSlopeScaleTexels
			};
			ResetPerBatchEnvProbeBox(constants);

			ctx.commandList.BindInputLayout(batchTransparent.mesh->layoutInstanced);
			ctx.commandList.BindVertexBuffer(0, batchTransparent.mesh->vertexBuffer, batchTransparent.mesh->vertexStrideBytes, 0);
			ctx.commandList.BindVertexBuffer(1, instanceBuffer_, instStride, batchTransparent.instanceOffset * instStride);
			ctx.commandList.BindIndexBuffer(batchTransparent.mesh->indexBuffer, batchTransparent.mesh->indexType, 0);

			ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &constants, 1 }));

			// IMPORTANT: transparent = one object per draw (instanceCount = 1)
			ctx.commandList.DrawIndexed(batchTransparent.mesh->indexCount, batchTransparent.mesh->indexType, 0, 0, 1, 0);
		}
	});
}
// --- Additive particles over deferred SceneColor ---
if (particleCount > 0u)
{
	renderGraph::PassAttachments att{};
	att.useSwapChainBackbuffer = false;
	att.colors = { sceneColorAfterFog };
	att.depth = depthRG;
	att.clearDesc.clearColor = false;
	att.clearDesc.clearDepth = false;
	att.clearDesc.clearStencil = false;

	graph.AddPass("DeferredParticles", std::move(att),
		[this, &scene, particleCount](renderGraph::PassContext& ctx)
		{
			const auto extent = ctx.passExtent;
			ctx.commandList.SetViewport(0, 0,
				static_cast<int>(extent.width),
				static_cast<int>(extent.height));

			const FrameCameraData camera = BuildFrameCameraData(scene, extent);
			DrawParticleBillboards(ctx.commandList, scene, camera, particleCount);
		});
}
// --- Editor selection (transparent) over deferred SceneColor ---
if (!selectionTransparent.empty())
{
	renderGraph::PassAttachments att{};
	att.useSwapChainBackbuffer = false;
	att.colors = { sceneColorAfterFog };
	att.depth = depthRG;
	att.clearDesc.clearColor = false;
	att.clearDesc.clearDepth = false;
	att.clearDesc.clearStencil = false;

	graph.AddPass("DeferredSelectionTransparent", std::move(att),
		[this, &scene,
		dirLightViewProj,
		selectionTransparent,
		selectionTransparentStart,
		selectionInstances,
		DrawEditorSelectionGroup,
		instStride](renderGraph::PassContext& ctx)
		{
			// Reuse the same implementation as DeferredSelectionOpaque by just delegating through the
			// exact same code path (outline+highlight), but after the transparent pass.
			// NOTE: we keep this as a separate pass so ordering stays correct.
			const auto extent = ctx.passExtent;
			ctx.commandList.SetViewport(0, 0,
				static_cast<int>(extent.width),
				static_cast<int>(extent.height));

			const FrameCameraData camera = BuildFrameCameraData(scene, extent);
			const mathUtils::Mat4& viewProj = camera.viewProj;
			const mathUtils::Vec3& camPosLocal = camera.camPos;
			const mathUtils::Vec3& camFLocal = camera.camForward;

			DrawEditorSelectionGroup(
				ctx,
				state_,
				mathUtils::Vec4(1.0f, 0.86f, 0.10f, 0.22f),
				viewProj,
				dirLightViewProj,
				camPosLocal,
				camFLocal,
				extent,
				selectionTransparent,
				selectionTransparentStart);
		});
}
auto finalSceneColor = sceneColorAfterFog;

if (settings_.enableHDR && settings_.enableBloom &&
	psoBloomExtract_ && psoBloomBlur_ && psoBloomComposite_)
{
	rhi::Extent2D bloomExtent{
		std::max(1u, scDesc.extent.width / 2u),
		std::max(1u, scDesc.extent.height / 2u)
	};

	const auto bloomExtract = graph.CreateTexture(renderGraph::RGTextureDesc{
		.extent = bloomExtent,
		.format = rhi::Format::RGBA16_FLOAT,
		.usage = renderGraph::ResourceUsage::RenderTarget,
		.debugName = "DeferredBloom_Extract"
		});

	const auto bloomBlurX = graph.CreateTexture(renderGraph::RGTextureDesc{
		.extent = bloomExtent,
		.format = rhi::Format::RGBA16_FLOAT,
		.usage = renderGraph::ResourceUsage::RenderTarget,
		.debugName = "DeferredBloom_BlurX"
		});

	const auto bloomBlurY = graph.CreateTexture(renderGraph::RGTextureDesc{
		.extent = bloomExtent,
		.format = rhi::Format::RGBA16_FLOAT,
		.usage = renderGraph::ResourceUsage::RenderTarget,
		.debugName = "DeferredBloom_BlurY"
		});

	const auto sceneColorBloom = graph.CreateTexture(renderGraph::RGTextureDesc{
		.extent = scDesc.extent,
		.format = sceneColorFormat,
		.usage = renderGraph::ResourceUsage::RenderTarget,
		.debugName = "DeferredSceneColor_Bloom"
		});

	{
		BloomExtractConstants c{};
		c.uInvSourceSize = {
			scDesc.extent.width ? (1.0f / static_cast<float>(scDesc.extent.width)) : 0.0f,
			scDesc.extent.height ? (1.0f / static_cast<float>(scDesc.extent.height)) : 0.0f,
			0.0f, 0.0f
		};
		c.uParams = {
			settings_.bloomThreshold,
			settings_.bloomSoftKnee,
			settings_.bloomClamp,
			0.0f
		};

		renderGraph::PassAttachments att{};
		att.useSwapChainBackbuffer = false;
		att.colors = { bloomExtract };
		att.clearDesc.clearColor = false;
		att.clearDesc.clearDepth = false;
		att.clearDesc.clearStencil = false;

		graph.AddPass("DeferredBloomExtract", std::move(att),
			[this, finalSceneColor, c](renderGraph::PassContext& ctx)
			{
				const auto extent = ctx.passExtent;
				ctx.commandList.SetViewport(0, 0, static_cast<int>(extent.width), static_cast<int>(extent.height));
				ctx.commandList.SetState(deferredLightingState_);
				ctx.commandList.BindPipeline(psoBloomExtract_);
				ctx.commandList.BindInputLayout(fullscreenLayout_);
				ctx.commandList.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
				ctx.commandList.BindTexture2D(0, ctx.resources.GetTexture(finalSceneColor));
				ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &c, 1 }));
				ctx.commandList.Draw(3);
			});
	}

	{
		BloomBlurConstants c{};
		c.uInvSourceSize = {
			bloomExtent.width ? (1.0f / static_cast<float>(bloomExtent.width)) : 0.0f,
			bloomExtent.height ? (1.0f / static_cast<float>(bloomExtent.height)) : 0.0f,
			0.0f, 0.0f
		};
		c.uDirection = { settings_.bloomRadius, 0.0f, 0.0f, 0.0f };

		renderGraph::PassAttachments att{};
		att.useSwapChainBackbuffer = false;
		att.colors = { bloomBlurX };
		att.clearDesc.clearColor = false;
		att.clearDesc.clearDepth = false;
		att.clearDesc.clearStencil = false;

		graph.AddPass("DeferredBloomBlurX", std::move(att),
			[this, bloomExtract, c](renderGraph::PassContext& ctx)
			{
				const auto extent = ctx.passExtent;
				ctx.commandList.SetViewport(0, 0, static_cast<int>(extent.width), static_cast<int>(extent.height));
				ctx.commandList.SetState(deferredLightingState_);
				ctx.commandList.BindPipeline(psoBloomBlur_);
				ctx.commandList.BindInputLayout(fullscreenLayout_);
				ctx.commandList.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
				ctx.commandList.BindTexture2D(0, ctx.resources.GetTexture(bloomExtract));
				ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &c, 1 }));
				ctx.commandList.Draw(3);
			});
	}

	{
		BloomBlurConstants c{};
		c.uInvSourceSize = {
			bloomExtent.width ? (1.0f / static_cast<float>(bloomExtent.width)) : 0.0f,
			bloomExtent.height ? (1.0f / static_cast<float>(bloomExtent.height)) : 0.0f,
			0.0f, 0.0f
		};
		c.uDirection = { 0.0f, settings_.bloomRadius, 0.0f, 0.0f };

		renderGraph::PassAttachments att{};
		att.useSwapChainBackbuffer = false;
		att.colors = { bloomBlurY };
		att.clearDesc.clearColor = false;
		att.clearDesc.clearDepth = false;
		att.clearDesc.clearStencil = false;

		graph.AddPass("DeferredBloomBlurY", std::move(att),
			[this, bloomBlurX, c](renderGraph::PassContext& ctx)
			{
				const auto extent = ctx.passExtent;
				ctx.commandList.SetViewport(0, 0, static_cast<int>(extent.width), static_cast<int>(extent.height));
				ctx.commandList.SetState(deferredLightingState_);
				ctx.commandList.BindPipeline(psoBloomBlur_);
				ctx.commandList.BindInputLayout(fullscreenLayout_);
				ctx.commandList.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
				ctx.commandList.BindTexture2D(0, ctx.resources.GetTexture(bloomBlurX));
				ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &c, 1 }));
				ctx.commandList.Draw(3);
			});
	}

	{
		BloomCompositeConstants c{};
		c.uParams = { settings_.bloomIntensity, 0.0f, 0.0f, 0.0f };

		renderGraph::PassAttachments att{};
		att.useSwapChainBackbuffer = false;
		att.colors = { sceneColorBloom };
		att.clearDesc.clearColor = false;
		att.clearDesc.clearDepth = false;
		att.clearDesc.clearStencil = false;

		graph.AddPass("DeferredBloomComposite", std::move(att),
			[this, finalSceneColor, bloomBlurY, c](renderGraph::PassContext& ctx)
			{
				const auto extent = ctx.passExtent;
				ctx.commandList.SetViewport(0, 0, static_cast<int>(extent.width), static_cast<int>(extent.height));
				ctx.commandList.SetState(deferredLightingState_);
				ctx.commandList.BindPipeline(psoBloomComposite_);
				ctx.commandList.BindInputLayout(fullscreenLayout_);
				ctx.commandList.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
				ctx.commandList.BindTexture2D(0, ctx.resources.GetTexture(finalSceneColor));
				ctx.commandList.BindTexture2D(1, ctx.resources.GetTexture(bloomBlurY));
				ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &c, 1 }));
				ctx.commandList.Draw(3);
			});
	}

	finalSceneColor = sceneColorBloom;
}

// --- Present: tonemap/copy SceneColor to swapchain ---
{
	rhi::ClearDesc clear{};
	clear.clearColor = false;
	clear.clearDepth = false;
	clear.clearStencil = false;

	const bool fxaaEnabled = settings_.antiAliasingMode == 1u && psoFXAA_ && fullscreenLayout_;


	if (fxaaEnabled)
	{
		const auto presentLdr = graph.CreateTexture(renderGraph::RGTextureDesc{
			.extent = scDesc.extent,
			.format = rhi::Format::RGBA8_UNORM,
			.usage = renderGraph::ResourceUsage::RenderTarget,
			.debugName = "DeferredPresentLdr"
			});

		renderGraph::PassAttachments att{};
		att.useSwapChainBackbuffer = false;
		att.colors = { presentLdr };
		att.clearDesc.clearColor = false;
		att.clearDesc.clearDepth = false;
		att.clearDesc.clearStencil = false;


		graph.AddPass("DeferredPresentLdr", std::move(att),
			[this, finalSceneColor](renderGraph::PassContext& ctx)
			{
				const auto extent = ctx.passExtent;

				ctx.commandList.SetViewport(0, 0,
					static_cast<int>(extent.width),
					static_cast<int>(extent.height));

				ctx.commandList.SetState(copyToSwapChainState_);
				ctx.commandList.BindInputLayout(fullscreenLayout_);
				ctx.commandList.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);

				if (psoToneMap_)
				{
					ToneMapConstants c{};
					c.uParams = {
						settings_.hdrExposure,
						static_cast<float>(settings_.toneMapMode),
						2.2f,
						settings_.enableHDR ? 1.0f : 0.0f
					};

					ctx.commandList.BindPipeline(psoToneMap_);
					ctx.commandList.BindTexture2D(0, ctx.resources.GetTexture(finalSceneColor));
					ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &c, 1 }));
					ctx.commandList.Draw(3, 0);
				}
				else
				{
					ctx.commandList.BindPipeline(psoCopyToSwapChain_);
					ctx.commandList.BindTexture2D(0, ctx.resources.GetTexture(finalSceneColor));
					ctx.commandList.Draw(3, 0);
				}
			});

		graph.AddSwapChainPass("DeferredPresentFXAA", clear,
			[this, presentLdr](renderGraph::PassContext& ctx)
			{
				const auto extent = ctx.passExtent;

				ctx.commandList.SetViewport(0, 0,
					static_cast<int>(extent.width),
					static_cast<int>(extent.height));

				ctx.commandList.SetState(copyToSwapChainState_);
				ctx.commandList.BindInputLayout(fullscreenLayout_);
				ctx.commandList.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);

				FXAAConstants c{};
				c.uInvSourceSize = {
					extent.width ? (1.0f / static_cast<float>(extent.width)) : 0.0f,
					extent.height ? (1.0f / static_cast<float>(extent.height)) : 0.0f,
					static_cast<float>(extent.width),
					static_cast<float>(extent.height)
				};

				c.uParams = {
					settings_.fxaaSubpix,
					settings_.fxaaEdgeThreshold,
					settings_.fxaaEdgeThresholdMin,
					0.0f
				};

				ctx.commandList.BindPipeline(psoFXAA_);
				ctx.commandList.BindTexture2D(0, ctx.resources.GetTexture(presentLdr));
				ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &c, 1 }));
				ctx.commandList.Draw(3, 0);
			});
	}
	else
	{
		graph.AddSwapChainPass("DeferredPresent", clear,
			[this, finalSceneColor](renderGraph::PassContext& ctx)
			{
				const auto extent = ctx.passExtent;

				ctx.commandList.SetViewport(0, 0,
					static_cast<int>(extent.width),
					static_cast<int>(extent.height));

				ctx.commandList.SetState(copyToSwapChainState_);
				ctx.commandList.BindInputLayout(fullscreenLayout_);
				ctx.commandList.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);

				if (psoToneMap_)
				{
					ToneMapConstants c{};
					c.uParams = {
						settings_.hdrExposure,
						static_cast<float>(settings_.toneMapMode),
						2.2f,
						settings_.enableHDR ? 1.0f : 0.0f
					};

					ctx.commandList.BindPipeline(psoToneMap_);
					ctx.commandList.BindTexture2D(0, ctx.resources.GetTexture(finalSceneColor));
					ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &c, 1 }));
					ctx.commandList.Draw(3, 0);
				}
				else
				{
					ctx.commandList.BindPipeline(psoCopyToSwapChain_);
					ctx.commandList.BindTexture2D(0, ctx.resources.GetTexture(finalSceneColor));
					ctx.commandList.Draw(3, 0);
				}
			});
	}
	// --- ImGui overlay (optional) ---
	if (imguiDrawData)
	{
		rhi::ClearDesc clear{};
		clear.clearColor = false;
		clear.clearDepth = false;
		clear.clearStencil = false;

		graph.AddSwapChainPass("DeferredImGui", clear,
			[this, imguiDrawData](renderGraph::PassContext& ctx)
			{
				ctx.commandList.DX12ImGuiRender(imguiDrawData);
			});
	}
}
}
