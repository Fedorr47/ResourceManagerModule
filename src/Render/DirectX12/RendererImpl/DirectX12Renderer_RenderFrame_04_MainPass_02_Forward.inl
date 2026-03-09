// ---------------- Forward path via offscreen SceneColor ----------------
const auto depthRG = graph.ImportTexture(
	swapChain.GetDepthTexture(),
	renderGraph::RGTextureDesc{
		.extent = scDesc.extent,
		.format = rhi::Format::D24_UNORM_S8_UINT,
		.usage = renderGraph::ResourceUsage::DepthStencil,
		.debugName = "SwapChainDepth_Imported_Forward"
	});

const auto sceneColorFormat = settings_.enableHDR ? rhi::Format::RGBA16_FLOAT : rhi::Format::RGBA8_UNORM;
const auto forwardSceneColor = graph.CreateTexture(renderGraph::RGTextureDesc{
	.extent = scDesc.extent,
	.format = sceneColorFormat,
	.usage = renderGraph::ResourceUsage::RenderTarget,
	.debugName = "ForwardSceneColor"
	});

auto forwardSceneColorAfterPost = forwardSceneColor;

bool canForwardSSAO =
device_.GetBackend() == rhi::Backend::DirectX12 &&
psoSSAOForward_ &&
psoSSAOBlur_ &&
psoSSAOComposite_ &&
fullscreenLayout_ &&
swapChain.GetDepthTexture();

//canForwardSSAO = false;

renderGraph::RGTextureHandle forwardSSAOBlur{};

if (canForwardSSAO)
{
	const auto ssaoRaw = graph.CreateTexture(renderGraph::RGTextureDesc{
		.extent = scDesc.extent,
		.format = rhi::Format::R32_FLOAT,
		.usage = renderGraph::ResourceUsage::RenderTarget,
		.debugName = "ForwardSSAO_Raw"
		});

	forwardSSAOBlur = graph.CreateTexture(renderGraph::RGTextureDesc{
		.extent = scDesc.extent,
		.format = rhi::Format::R32_FLOAT,
		.usage = renderGraph::ResourceUsage::RenderTarget,
		.debugName = "ForwardSSAO_Blur"
		});


	renderGraph::PassAttachments att{};
	att.useSwapChainBackbuffer = false;
	att.colors = { ssaoRaw };
	att.clearDesc.clearColor = true;
	att.clearDesc.color = { 1.0f, 1.0f, 1.0f, 1.0f };

	graph.AddPass("ForwardSSAO", std::move(att),
		[this, &scene, depthRG](renderGraph::PassContext& ctx)
		{
			const auto extent = ctx.passExtent;
			ctx.commandList.SetViewport(0, 0, static_cast<int>(extent.width), static_cast<int>(extent.height));

			const FrameCameraData camera = BuildFrameCameraData(scene, extent);
			const mathUtils::Mat4& invViewProjT = camera.invViewProjT;

			SSAOConstants c{};
			std::memcpy(c.uInvViewProj.data(), mathUtils::ValuePtr(invViewProjT), sizeof(float) * 16);
			c.uParams = {
				settings_.enableSSAO ? settings_.ssaoRadius : 0.0f,
				settings_.ssaoBias,
				settings_.ssaoStrength,
				settings_.ssaoPower
			};
			c.uInvSize = {
				extent.width ? (1.0f / static_cast<float>(extent.width)) : 0.0f,
				extent.height ? (1.0f / static_cast<float>(extent.height)) : 0.0f,
				0.0f, 0.0f
			};

			ctx.commandList.SetState(deferredLightingState_);
			ctx.commandList.BindPipeline(psoSSAOForward_);
			ctx.commandList.BindInputLayout(fullscreenLayout_);
			ctx.commandList.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
			ctx.commandList.BindTexture2D(0, ctx.resources.GetTexture(depthRG));
			ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &c, 1 }));
			ctx.commandList.Draw(3);
		});

	renderGraph::PassAttachments blurAtt{};
	blurAtt.useSwapChainBackbuffer = false;
	blurAtt.colors = { forwardSSAOBlur };
	blurAtt.clearDesc.clearColor = true;
	blurAtt.clearDesc.color = { 1.0f, 1.0f, 1.0f, 1.0f };

	graph.AddPass("ForwardSSAOBlur", std::move(blurAtt),
		[this, depthRG, ssaoRaw, forwardSSAOBlur](renderGraph::PassContext& ctx)
		{
			const auto extent = ctx.passExtent;
			ctx.commandList.SetViewport(0, 0, static_cast<int>(extent.width), static_cast<int>(extent.height));

			SSAOBlurConstants c{};
			c.uInvSize = {
				extent.width ? (1.0f / static_cast<float>(extent.width)) : 0.0f,
				extent.height ? (1.0f / static_cast<float>(extent.height)) : 0.0f,
				0.0f, 0.0f
			};
			c.uParams = { settings_.ssaoBlurDepthThreshold, 0.0f, 0.0f, 0.0f };

			ctx.commandList.SetState(deferredLightingState_);
			ctx.commandList.BindPipeline(psoSSAOBlur_);
			ctx.commandList.BindInputLayout(fullscreenLayout_);
			ctx.commandList.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
			ctx.commandList.BindTexture2D(0, ctx.resources.GetTexture(ssaoRaw));
			ctx.commandList.BindTexture2D(1, ctx.resources.GetTexture(depthRG));
			ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &c, 1 }));
			ctx.commandList.Draw(3);
		});
}

// ---------------- Main pass (swapchain) ----------------
rhi::ClearDesc clearDesc{};
clearDesc.clearColor = true;
clearDesc.clearDepth = !doDepthPrepass; // if we pre-filled depth, don't wipe it here
clearDesc.clearStencil = true;
clearDesc.stencil = 0;
clearDesc.color = { 0.1f, 0.1f, 0.1f, 1.0f };

renderGraph::PassAttachments mainAtt{};
mainAtt.useSwapChainBackbuffer = false;
mainAtt.colors = { forwardSceneColor };
mainAtt.depth = depthRG;
mainAtt.clearDesc = clearDesc;

graph.AddPass("ForwardOpaquePass", std::move(mainAtt), [
	this,
	&scene,
	shadowRG,
	dirLightViewProj,
	lightCount,
	spotShadows,
	pointShadows,
	mainBatches,
	instStride,
	activeReflectionProbeCount,
	ResolveMainPassMaterialPerm,
	ResolveOpaqueEnvBinding,
	BindMainPassMaterialTextures,
	BuildMainPassMaterialFlags,
	ComputeForwardGBufferReflectionMeta,
	doDepthPrepass](renderGraph::PassContext& ctx)
{
	const auto extent = ctx.passExtent;

	ctx.commandList.SetViewport(0, 0,
		static_cast<int>(extent.width),
		static_cast<int>(extent.height));

	// If we ran a depth prepass, keep depth read-only in the main pass.
	ctx.commandList.SetState(doDepthPrepass ? mainAfterPreDepthState_ : state_);

	const FrameCameraData camera = BuildFrameCameraData(scene, extent);
	const mathUtils::Mat4& proj = camera.proj;
	const mathUtils::Mat4& view = camera.view;
	const mathUtils::Mat4& viewProj = camera.viewProj;
	const mathUtils::Vec3& camPosLocal = camera.camPos;
	const mathUtils::Vec3& camFLocal = camera.camForward;

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

	for (const Batch& batch : mainBatches)
	{
		if (!batch.mesh || batch.instanceCount == 0)
		{
			continue;
		}

		const MaterialPerm perm = ResolveMainPassMaterialPerm(batch.material, batch.materialHandle);
		const bool useTex = HasFlag(perm, MaterialPerm::UseTex);
		const bool useShadow = HasFlag(perm, MaterialPerm::UseShadow);
		const ResolvedMaterialEnvBinding env = ResolveOpaqueEnvBinding(
			batch.materialHandle,
			batch.reflectionProbeIndex);

		ctx.commandList.BindPipeline(MainPipelineFor(perm));
		BindMainPassMaterialTextures(ctx.commandList, batch.material, env);

		const std::uint32_t flags = BuildMainPassMaterialFlags(
			batch.material,
			useTex,
			useShadow,
			env);

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

		const auto [envSourceForGBuffer, probeIdxNForGBuffer] = ComputeForwardGBufferReflectionMeta(
			batch.materialHandle,
			batch.reflectionProbeIndex,
			activeReflectionProbeCount);

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

		constants.uEnvProbeBoxMin = { 0.0f, 0.0f, 0.0f, envSourceForGBuffer };
		constants.uEnvProbeBoxMax = { 0.0f, 0.0f, 0.0f, probeIdxNForGBuffer };

		// IA (instanced)
		ctx.commandList.BindInputLayout(batch.mesh->layoutInstanced);
		ctx.commandList.BindVertexBuffer(0, batch.mesh->vertexBuffer, batch.mesh->vertexStrideBytes, 0);
		ctx.commandList.BindVertexBuffer(1, instanceBuffer_, instStride, batch.instanceOffset * instStride);
		ctx.commandList.BindIndexBuffer(batch.mesh->indexBuffer, batch.mesh->indexType, 0);

		ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &constants, 1 }));
		ctx.commandList.DrawIndexed(batch.mesh->indexCount, batch.mesh->indexType, 0, 0, batch.instanceCount, 0);
	}
	});

	// Planar reflections in the forward path cannot safely render straight into the main depth buffer:
	// reflected geometry lives "behind" the mirror plane in camera space, so reusing the main depth
	// buffer causes depth conflicts with real scene geometry. Use the same offscreen mask+reflColor+
	// reflDepth path as deferred, then composite back into ForwardSceneColor.
	const auto sceneColor = forwardSceneColor;
#include "RendererImpl/DirectX12Renderer_RenderFrame_04b_PlanarReflections_MaskRT.inl"

	renderGraph::PassAttachments transparentAtt{};
	transparentAtt.useSwapChainBackbuffer = false;
	transparentAtt.colors = { forwardSceneColor };
	transparentAtt.depth = depthRG;
	transparentAtt.clearDesc.clearColor = false;
	transparentAtt.clearDesc.clearDepth = false;
	transparentAtt.clearDesc.clearStencil = false;

	graph.AddPass("ForwardTransparentPass", std::move(transparentAtt), [
		this,
		&scene,
		shadowRG,
		dirLightViewProj,
		lightCount,
		spotShadows,
		pointShadows,
		instStride,
		transparentDraws,
		selectionOpaque,
		selectionOpaqueStart,
		selectionTransparent,
		selectionTransparentStart,
		DrawEditorSelectionGroup,
		ResolveMainPassMaterialPerm,
		ResolveTransparentEnvBinding,
		BindMainPassMaterialTextures,
		BuildMainPassMaterialFlags,
		FillPerBatchViewLightingConstants,
		ResetPerBatchEnvProbeBox,
		particleCount,
		doDepthPrepass](renderGraph::PassContext& ctx)
	{
		const auto extent = ctx.passExtent;

		ctx.commandList.SetViewport(0, 0,
			static_cast<int>(extent.width),
			static_cast<int>(extent.height));

		const FrameCameraData camera = BuildFrameCameraData(scene, extent);
		const mathUtils::Mat4& viewProj = camera.viewProj;
		const mathUtils::Vec3& camPosLocal = camera.camPos;
		const mathUtils::Vec3& camFLocal = camera.camForward;

		// Rebind lighting/shadow resources for transparent draws.
		{
			const auto shadowTex = ctx.resources.GetTexture(shadowRG);
			ctx.commandList.BindTexture2D(1, shadowTex);
		}
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
		ctx.commandList.BindStructuredBufferSRV(11, shadowDataBuffer_);
		ctx.commandList.BindStructuredBufferSRV(2, lightsBuffer_);

	// If selected objects are opaque, draw outline/highlight BEFORE transparent objects
	// so transparent surfaces still blend on top.
	if (!selectionOpaque.empty())
	{
		DrawEditorSelectionGroup(
			ctx,
			doDepthPrepass ? mainAfterPreDepthState_ : state_,
			mathUtils::Vec4(1.0f, 0.95f, 0.25f, 0.35f),
			viewProj,
			dirLightViewProj,
			camPosLocal,
			camFLocal,
			extent,
			selectionOpaque,
			selectionOpaqueStart);
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
				0.0f
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
	}
	if (particleCount > 0u)
	{
		DrawParticleBillboards(ctx.commandList, scene, camera, particleCount);
	}

	// If selected objects are transparent, render outline/highlight AFTER the transparent pass
	// so it stays visible on top of their own translucency.
	if (!selectionTransparent.empty())
	{
		DrawEditorSelectionGroup(
			ctx,
			doDepthPrepass ? mainAfterPreDepthState_ : state_,
			mathUtils::Vec4(1.0f, 0.95f, 0.25f, 0.35f),
			viewProj,
			dirLightViewProj,
			camPosLocal,
			camFLocal,
			extent,
			selectionTransparent,
			selectionTransparentStart);
	}

});

if (canForwardSSAO)
{
	const auto sceneColorAO = graph.CreateTexture(renderGraph::RGTextureDesc{
		.extent = scDesc.extent,
		.format = sceneColorFormat,
		.usage = renderGraph::ResourceUsage::RenderTarget,
		.debugName = "ForwardSceneColor_AO"
		});

	renderGraph::PassAttachments aoAtt{};
	aoAtt.useSwapChainBackbuffer = false;
	aoAtt.colors = { sceneColorAO };
	aoAtt.clearDesc.clearColor = false;
	aoAtt.clearDesc.clearDepth = false;
	aoAtt.clearDesc.clearStencil = false;

	const auto sceneColorIn = forwardSceneColorAfterPost;
	graph.AddPass("ForwardSSAOComposite", std::move(aoAtt),
		[this, sceneColorIn, forwardSSAOBlur](renderGraph::PassContext& ctx)
		{
			const auto extent = ctx.passExtent;
			ctx.commandList.SetViewport(0, 0, static_cast<int>(extent.width), static_cast<int>(extent.height));
			ctx.commandList.SetState(deferredLightingState_);
			ctx.commandList.BindPipeline(psoSSAOComposite_);
			ctx.commandList.BindInputLayout(fullscreenLayout_);
			ctx.commandList.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
			ctx.commandList.BindTexture2D(0, ctx.resources.GetTexture(sceneColorIn));
			ctx.commandList.BindTexture2D(1, ctx.resources.GetTexture(forwardSSAOBlur));
			ctx.commandList.Draw(3);
		});

	forwardSceneColorAfterPost = sceneColorAO;
}

if (settings_.enableFog && psoFog_)
{
	const FrameCameraData camera = BuildFrameCameraData(scene, scDesc.extent);

	FogConstants c{};
	const mathUtils::Mat4& invViewProjT = camera.invViewProjT;
	std::memcpy(c.uInvViewProj.data(), mathUtils::ValuePtr(invViewProjT), sizeof(float) * 16);
	c.uCameraPos = { camera.camPos.x, camera.camPos.y, camera.camPos.z, 0.0f };
	c.uFogParams = { settings_.fogStart, settings_.fogEnd, settings_.fogDensity, static_cast<float>(settings_.fogMode) };
	c.uFogColor = { settings_.fogColor[0], settings_.fogColor[1], settings_.fogColor[2], settings_.enableFog ? 1.0f : 0.0f };

	const auto sceneColorFog = graph.CreateTexture(renderGraph::RGTextureDesc{
		.extent = scDesc.extent,
		.format = sceneColorFormat,
		.usage = renderGraph::ResourceUsage::RenderTarget,
		.debugName = "ForwardSceneColor_Fog"
		});

	renderGraph::PassAttachments fogAtt{};
	fogAtt.useSwapChainBackbuffer = false;
	fogAtt.colors = { sceneColorFog };
	fogAtt.clearDesc.clearColor = false;
	fogAtt.clearDesc.clearDepth = false;
	fogAtt.clearDesc.clearStencil = false;

	const auto sceneColorIn = forwardSceneColorAfterPost;
	graph.AddPass("ForwardFog", std::move(fogAtt),
		[this, depthRG, sceneColorIn, c](renderGraph::PassContext& ctx)
		{
			const auto extent = ctx.passExtent;
			ctx.commandList.SetViewport(0, 0, static_cast<int>(extent.width), static_cast<int>(extent.height));
			ctx.commandList.SetState(deferredLightingState_);
			ctx.commandList.BindPipeline(psoFog_);
			ctx.commandList.BindInputLayout(fullscreenLayout_);
			ctx.commandList.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
			ctx.commandList.BindTexture2D(0, ctx.resources.GetTexture(sceneColorIn));
			ctx.commandList.BindTexture2D(1, ctx.resources.GetTexture(depthRG));
			ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &c, 1 }));
			ctx.commandList.Draw(3);
		});

	forwardSceneColorAfterPost = sceneColorFog;
}

auto finalSceneColor = forwardSceneColorAfterPost;

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
		.debugName = "ForwardBloom_Extract"
		});

	const auto bloomBlurX = graph.CreateTexture(renderGraph::RGTextureDesc{
		.extent = bloomExtent,
		.format = rhi::Format::RGBA16_FLOAT,
		.usage = renderGraph::ResourceUsage::RenderTarget,
		.debugName = "ForwardBloom_BlurX"
		});

	const auto bloomBlurY = graph.CreateTexture(renderGraph::RGTextureDesc{
		.extent = bloomExtent,
		.format = rhi::Format::RGBA16_FLOAT,
		.usage = renderGraph::ResourceUsage::RenderTarget,
		.debugName = "ForwardBloom_BlurY"
		});

	const auto sceneColorBloom = graph.CreateTexture(renderGraph::RGTextureDesc{
		.extent = scDesc.extent,
		.format = sceneColorFormat,
		.usage = renderGraph::ResourceUsage::RenderTarget,
		.debugName = "ForwardSceneColor_Bloom"
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

		graph.AddPass("ForwardBloomExtract", std::move(att),
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

		graph.AddPass("ForwardBloomBlurX", std::move(att),
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

		graph.AddPass("ForwardBloomBlurY", std::move(att),
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

		graph.AddPass("ForwardBloomComposite", std::move(att),
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
			.debugName = "ForwardPresentLdr"
			});

		renderGraph::PassAttachments att{};
		att.useSwapChainBackbuffer = false;
		att.colors = { presentLdr };
		att.clearDesc.clearColor = false;
		att.clearDesc.clearDepth = false;
		att.clearDesc.clearStencil = false;

		graph.AddPass("ForwardPresentLdr", std::move(att), [this, finalSceneColor](renderGraph::PassContext& ctx)
			{
				const auto extent = ctx.passExtent;
				ctx.commandList.SetViewport(0, 0, static_cast<int>(extent.width), static_cast<int>(extent.height));
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

		graph.AddSwapChainPass("ForwardPresentFXAA", clear, [this, presentLdr](renderGraph::PassContext& ctx)
			{
				const auto extent = ctx.passExtent;
				ctx.commandList.SetViewport(0, 0, static_cast<int>(extent.width), static_cast<int>(extent.height));
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
		graph.AddSwapChainPass("ForwardPresent", clear, [this, finalSceneColor](renderGraph::PassContext& ctx)
			{
				const auto extent = ctx.passExtent;
				ctx.commandList.SetViewport(0, 0, static_cast<int>(extent.width), static_cast<int>(extent.height));
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

	if (imguiDrawData)
	{
		rhi::ClearDesc clear{};
		clear.clearColor = false;
		clear.clearDepth = false;
		clear.clearStencil = false;

		graph.AddSwapChainPass("ForwardImGui", clear, [this, imguiDrawData](renderGraph::PassContext& ctx)
			{
				ctx.commandList.DX12ImGuiRender(imguiDrawData);
			});
	}
}
}