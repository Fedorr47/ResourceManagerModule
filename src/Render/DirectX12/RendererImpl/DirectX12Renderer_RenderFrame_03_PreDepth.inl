// ---------------- Optional depth pre-pass (swapchain depth) ----------------
const bool doDepthPrepass = settings_.enableDepthPrepass && !settings_.enableDeferred;
if (doDepthPrepass && psoShadow_)
{
	// We use the existing depth-only shadow shader (writes SV_Depth, no color outputs).
	// It expects a single matrix (uLightViewProj), so we feed it the camera view-projection.
	rhi::ClearDesc preClear{};
	preClear.clearColor = false; // keep backbuffer untouched
	preClear.clearDepth = true;
	preClear.depth = 1.0f;

	graph.AddSwapChainPass("PreDepthPass", preClear,
		[this, &scene, shadowBatches, instStride](renderGraph::PassContext& ctx) mutable
		{
			const auto extent = ctx.passExtent;
			ctx.commandList.SetViewport(0, 0,
				static_cast<int>(extent.width),
				static_cast<int>(extent.height));

			// Pre-depth state: depth test+write, opaque raster.
			ctx.commandList.SetState(preDepthState_);
			ctx.commandList.BindPipeline(psoShadow_);

			const FrameCameraData camera = BuildFrameCameraData(scene, extent);

			SingleMatrixPassConstants c{};
			const mathUtils::Mat4 vpT = mathUtils::Transpose(camera.viewProj);
			std::memcpy(c.uLightViewProj.data(), mathUtils::ValuePtr(vpT), sizeof(float) * 16);
			ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &c, 1 }));

			this->DrawInstancedShadowBatches(ctx.commandList, shadowBatches, instStride);
		});
}