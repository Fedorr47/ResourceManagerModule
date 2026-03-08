// --- Skybox after lighting: fills background where depth==1 (render into SceneColor) ---
{
	renderGraph::PassAttachments att{};
	att.useSwapChainBackbuffer = false;
	att.colors = { sceneColorAfterFog };

	att.depth = depthRG;

	att.clearDesc.clearColor = false;
	att.clearDesc.clearDepth = false;
	att.clearDesc.clearStencil = false;

	graph.AddPass("DeferredSkybox", std::move(att),
		[this, &scene, instStride, DrawEditorSelectionGroup](renderGraph::PassContext& ctx)
		{
			const auto extent = ctx.passExtent;

			const FrameCameraData camera = BuildFrameCameraData(scene, extent);
			const mathUtils::Mat4& proj = camera.proj;
			const mathUtils::Mat4& view = camera.view;

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
			}
		});
	}

// --- Planar reflections (mask RT path) ---
#include "RendererImpl/DirectX12Renderer_RenderFrame_04b_PlanarReflections_MaskRT.inl"

		// --- Fog (post effect): SceneColor + depth -> SceneColor_Fog ---
if (settings_.enableFog && psoFog_)
{
	const auto sceneColorFog = graph.CreateTexture(renderGraph::RGTextureDesc{
		.extent = scDesc.extent,
		.format = sceneColorFormat,
		.usage = renderGraph::ResourceUsage::RenderTarget,
		.debugName = "SceneColor_Fog"
		});


	FogConstants c{};
	std::memcpy(c.uInvViewProj.data(), mathUtils::ValuePtr(invViewProjT), sizeof(float) * 16);
	c.uCameraPos = { camPosLocal.x, camPosLocal.y, camPosLocal.z, 0.0f };
	c.uFogParams = { settings_.fogStart, settings_.fogEnd, settings_.fogDensity, static_cast<float>(settings_.fogMode) };
	c.uFogColor = {
		settings_.fogColor[0],
		settings_.fogColor[1],
		settings_.fogColor[2],
		settings_.enableFog ? 1.0f : 0.0f
	};

	renderGraph::PassAttachments att{};
	att.useSwapChainBackbuffer = false;
	att.colors = { sceneColorFog };

	att.clearDesc.clearColor = false;
	att.clearDesc.clearDepth = false;
	att.clearDesc.clearStencil = false;

	const auto sceneColorIn = sceneColorAfterFog;
	graph.AddPass("DeferredFog", std::move(att),
		[this, depthRG, sceneColorIn, sceneColorFog, c](renderGraph::PassContext& ctx)
		{
			const auto extent = ctx.passExtent;

			ctx.commandList.SetViewport(0, 0,
				static_cast<int>(extent.width),
				static_cast<int>(extent.height));

			ctx.commandList.SetState(deferredLightingState_);
			ctx.commandList.BindPipeline(psoFog_);
			ctx.commandList.BindInputLayout(fullscreenLayout_);
			ctx.commandList.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);

			// t0 = SceneColor, t1 = depth
			ctx.commandList.BindTexture2D(0, ctx.resources.GetTexture(sceneColorIn));
			ctx.commandList.BindTexture2D(1, ctx.resources.GetTexture(depthRG));
			ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &c, 1 }));
			ctx.commandList.Draw(3);
		});

	sceneColorAfterFog = sceneColorFog;
}