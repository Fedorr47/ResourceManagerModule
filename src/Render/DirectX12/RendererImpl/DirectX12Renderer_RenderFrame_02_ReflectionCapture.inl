// ---------------- ReflectionCapture pass (cubemap) ----------------
// Uses capture shaders/PSOs from 0007.
// Method selection:
//  - Layered: SV_RenderTargetArrayIndex (requires VPAndRTArrayIndexFromAnyShader + PSO)
//  - VI:      SV_ViewID (requires ViewInstancing + PSO)
//  - Fallback: 6 passes, one face at a time

/*
if (settings_.enableReflectionCapture && reflectionCube_ && psoReflectionCapture_)
{
	// Decide capture position.
	// We want the capture centered on the reflective object (probe anchor), not on the camera.
	// Priority:
	//  1) Follow editor selection (if enabled and valid)
	//  2) Otherwise, auto-pick the closest draw item that uses EnvSource::ReflectionCapture (typically MirrorSphere)
	//  3) Fallback to camera position if nothing matches
	mathUtils::Vec3 capturePos = camPos;
	int captureAnchorDrawItem = -1;

	// Follow selected object (editor selection).
	const int selectedDrawItem = scene.editorSelectedDrawItem;
	if (settings_.reflectionCaptureFollowSelectedObject && selectedDrawItem >= 0
		&& static_cast<std::size_t>(selectedDrawItem) < scene.drawItems.size())
	{
		captureAnchorDrawItem = selectedDrawItem;
		const auto& it = scene.drawItems[static_cast<std::size_t>(selectedDrawItem)];
		capturePos = it.transform.position;
	}
	else
	{
		float bestDist2 = 3.402823466e+38f; // FLT_MAX

		for (std::size_t i = 0; i < scene.drawItems.size(); ++i)
		{
			const auto& it = scene.drawItems[i];
			if (it.material.id == 0)
				continue;

			const auto& mat = scene.GetMaterial(it.material);
			if (mat.envSource != EnvSource::ReflectionCapture)
				continue;

			const mathUtils::Vec3 d = it.transform.position - camPos;
			const float dist2 = mathUtils::Dot(d, d);
			if (dist2 < bestDist2)
			{
				bestDist2 = dist2;
				captureAnchorDrawItem = static_cast<int>(i);
				capturePos = it.transform.position;
			}
		}
	}

	// Dirty logic: anchor change or movement.
	if (captureAnchorDrawItem != reflectionCaptureLastSelectedDrawItem_)
	{
		reflectionCaptureLastSelectedDrawItem_ = captureAnchorDrawItem;
		reflectionCaptureDirty_ = true;
		reflectionCaptureHasLastPos_ = false;
	}

	if (!reflectionCaptureHasLastPos_
		|| mathUtils::Length(capturePos - reflectionCaptureLastPos_) > 1e-4f)
	{
		reflectionCaptureDirty_ = true;
	}

	const bool doUpdate =
		settings_.reflectionCaptureUpdateEveryFrame ||
		reflectionCaptureDirty_;

	if (doUpdate)
	{
		reflectionCaptureDirty_ = false;
		reflectionCaptureHasLastPos_ = true;
		reflectionCaptureLastPos_ = capturePos;

		// Import persistent cube textures to render graph.
		const auto cubeRG = graph.ImportTexture(reflectionCube_, renderGraph::RGTextureDesc{
			.extent = reflectionCubeExtent_,
			.format = rhi::Format::RGBA8_UNORM,
			.usage = renderGraph::ResourceUsage::RenderTarget,
			.type = renderGraph::TextureType::Cube,
			.debugName = "ReflectionCaptureCube"
			});

		const auto depthCubeRG = graph.ImportTexture(reflectionDepthCube_, renderGraph::RGTextureDesc{
			.extent = reflectionCubeExtent_,
			.format = rhi::Format::D32_FLOAT,
			.usage = renderGraph::ResourceUsage::DepthStencil,
			.type = renderGraph::TextureType::Cube,
			.debugName = "ReflectionCaptureDepthCube"
			});

		// Capabilities / method choice.
		bool useLayered =
			(!disableReflectionCaptureLayered_) &&
			(psoReflectionCaptureLayered_) &&
			device_.SupportsShaderModel6() &&
			device_.SupportsVPAndRTArrayIndexFromAnyShader();

		bool useVI =
			(!useLayered) &&
			(!disableReflectionCaptureVI_) &&
			(psoReflectionCaptureVI_) &&
			device_.SupportsShaderModel6() &&
			device_.SupportsViewInstancing();	

		// Common face view helper for cubemap capture. Note: we keep it consistent with Skybox_dx12.hlsl (which flips Z when sampling the skybox cubemap).
		auto FaceView = [](const mathUtils::Vec3& pos, int face) -> mathUtils::Mat4
			{
				// +X, -X, +Y, -Y, +Z, -Z
				static const mathUtils::Vec3 dirs[6] = {
					{ 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }
				};

				static const mathUtils::Vec3 ups[6] = {
					{ 0, 1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }, { 0, 1, 0 }, { 0, 1, 0 }
				};
				return mathUtils::LookAtRH(pos, pos + dirs[face], ups[face]);
			};

		const float nearZ = std::max(0.001f, settings_.reflectionCaptureNearZ);
		const float farZ = std::max(nearZ + 0.01f, settings_.reflectionCaptureFarZ);
		const mathUtils::Mat4 proj90 = mathUtils::PerspectiveRH_ZO(mathUtils::DegToRad(90.0f), 1.0f, nearZ, farZ);

		// Clear (two variants: full clear for skybox/background, depth-only for overlaying meshes)

		rhi::ClearDesc clearColorDepth{};
		clearColorDepth.clearColor = true;
		clearColorDepth.clearDepth = true;
		clearColorDepth.color = { 0.0f, 0.0f, 0.0f, 1.0f };
		clearColorDepth.depth = 1.0f;

		rhi::ClearDesc clearDepthOnly{};
		clearDepthOnly.clearColor = false;
		clearDepthOnly.clearDepth = true;
		clearDepthOnly.depth = 1.0f;

		// Optional: render skybox into capture cube first (so reflections have a proper background).
		const std::uint32_t skyboxDesc = scene.skyboxDescIndex;
		const bool haveSkybox = (skyboxDesc != 0);

		// A temporary per-face depth buffer for fallback (and skybox) passes.
		const auto depthTmp = graph.CreateTexture(renderGraph::RGTextureDesc{
			.extent = reflectionCubeExtent_,
			.format = rhi::Format::D32_FLOAT,
			.usage = renderGraph::ResourceUsage::DepthStencil,
			.debugName = "ReflectionCaptureDepthTmp"
			});

		bool renderedSkybox = false;
		
		if (haveSkybox)
		{
			renderedSkybox = true;
			for (int face = 0; face < 6; ++face)
			{
				renderGraph::PassAttachments att{};
				att.useSwapChainBackbuffer = false;
				att.colorCubeFace = static_cast<std::uint32_t>(face);
				att.color = cubeRG;
				att.depth = depthTmp;
				att.clearDesc = clearColorDepth;

				mathUtils::Mat4 view = FaceView(capturePos, face);
				view[3] = mathUtils::Vec4(0, 0, 0, 1);

				const mathUtils::Mat4 viewProjSkybox = proj90 * view;
				const mathUtils::Mat4 viewProjSkyboxTranspose = mathUtils::Transpose(viewProjSkybox);

				SkyboxConstants skyboxConstants{};
				std::memcpy(skyboxConstants.uViewProj.data(), mathUtils::ValuePtr(viewProjSkyboxTranspose), sizeof(float) * 16);

				const std::string passName = "ReflectionCapture_Skybox_Face_" + std::to_string(face);
				graph.AddPass(passName, std::move(att),
					[this, skyboxDesc, skyboxConstants](renderGraph::PassContext& ctx) mutable
					{
						ctx.commandList.SetViewport(0, 0, (int)ctx.passExtent.width, (int)ctx.passExtent.height);
						ctx.commandList.SetState(skyboxState_);
						ctx.commandList.BindPipeline(psoSkybox_);
						ctx.commandList.BindTextureDesc(0, skyboxDesc);

						ctx.commandList.BindInputLayout(skyboxMesh_.layout);
						ctx.commandList.BindVertexBuffer(0, skyboxMesh_.vertexBuffer, skyboxMesh_.vertexStrideBytes, 0);
						ctx.commandList.BindIndexBuffer(skyboxMesh_.indexBuffer, skyboxMesh_.indexType, 0);

						ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &skyboxConstants, 1 }));
						ctx.commandList.DrawIndexed(skyboxMesh_.indexCount, skyboxMesh_.indexType, 0, 0);

					});
			}
		}

		// For mesh passes: if we rendered skybox first, don't clear color again.
		const rhi::ClearDesc meshClear = renderedSkybox ? clearDepthOnly : clearColorDepth;

		// Avoid recursive self-capture:
		// Do not render objects that themselves use EnvSource::ReflectionCapture into the capture cubemap.
		auto ShouldSkipInCapture = [&scene](const Batch& b) -> bool
			{
				if (b.materialHandle.id == 0)
					return false;

				const auto& mat = scene.GetMaterial(b.materialHandle);
				return (mat.envSource == EnvSource::ReflectionCapture);
			};

		std::vector<Batch> captureMainBatches;
		captureMainBatches.reserve(mainBatches.size());
		for (const Batch& b : mainBatches)
		{
			if (!ShouldSkipInCapture(b))
				captureMainBatches.push_back(b);
		}

		std::vector<Batch> captureReflectionBatchesLayered;
		captureReflectionBatchesLayered.reserve(reflectionBatchesLayered.size());
		for (const Batch& b : reflectionBatchesLayered)
		{
			if (!ShouldSkipInCapture(b))
				captureReflectionBatchesLayered.push_back(b);
		}

		// ---------------- Layered path (one pass, SV_RenderTargetArrayIndex) ----------------
		if (useLayered && !captureReflectionBatchesLayered.empty())
		{
			renderGraph::PassAttachments att{};
			att.useSwapChainBackbuffer = false;
			att.color = cubeRG;
			att.colorCubeAllFaces = true;
			att.depth = depthCubeRG;
			att.clearDesc = meshClear;

			// Precompute face matrices once (stored transposed).
			ReflectionCaptureConstants base{};
			for (int face = 0; face < 6; ++face)
			{
				const mathUtils::Mat4 vp = proj90 * FaceView(capturePos, face);
				const mathUtils::Mat4 vpT = mathUtils::Transpose(vp);
				std::memcpy(base.uFaceViewProj.data() + face * 16, mathUtils::ValuePtr(vpT), sizeof(float) * 16);
			}

			// capturePos.xyz + ambientStrength
			base.uCapturePosAmbient = { capturePos.x, capturePos.y, capturePos.z, 0.22f };
			base.uParams = { static_cast<float>(lightCount), 0.0f, 0.0f, 0.0f };

			graph.AddPass("ReflectionCapture_Layered", std::move(att),
				[this, base, lightCount, instStride, captureReflectionBatchesLayered](renderGraph::PassContext& ctx) mutable
				{
					ctx.commandList.SetViewport(0, 0, (int)ctx.passExtent.width, (int)ctx.passExtent.height);
					ctx.commandList.SetState(state_);
					ctx.commandList.BindPipeline(psoReflectionCaptureLayered_);
					ctx.commandList.BindStructuredBufferSRV(2, lightsBuffer_);

					for (const Batch& b : captureReflectionBatchesLayered)
					{
						if (!b.mesh || b.instanceCount == 0)
							continue;

						assert((b.instanceOffset % 6u) == 0u);
						assert((b.instanceCount % 6u) == 0u);

						// flags
						std::uint32_t flags = 0u;
						const bool useTex = (b.materialHandle.id != 0) && (b.material.albedoDescIndex != 0);
						if (useTex) flags |= 1u; // FLAG_USE_TEX

						// bind albedo (t0)
						ctx.commandList.BindTextureDesc(0, useTex ? b.material.albedoDescIndex : 0);

						ReflectionCaptureConstants c = base;
						c.uBaseColor = { b.material.baseColor.x, b.material.baseColor.y, b.material.baseColor.z, b.material.baseColor.w };
						c.uParams[1] = AsFloatBits(flags);

						ctx.commandList.BindInputLayout(b.mesh->layoutInstanced);
						ctx.commandList.BindVertexBuffer(0, b.mesh->vertexBuffer, b.mesh->vertexStrideBytes, 0);
						ctx.commandList.BindVertexBuffer(1, instanceBuffer_, instStride, b.instanceOffset * instStride);
						ctx.commandList.BindIndexBuffer(b.mesh->indexBuffer, b.mesh->indexType, 0);

						ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &c, 1 }));
						ctx.commandList.DrawIndexed(b.mesh->indexCount, b.mesh->indexType, 0, 0, b.instanceCount, 0);
					}
				});
		}
		// ---------------- VI path (one pass, SV_ViewID) ----------------
		else if (useVI)
		{
			renderGraph::PassAttachments att{};
			att.useSwapChainBackbuffer = false;
			att.color = cubeRG;
			att.colorCubeAllFaces = true;
			att.depth = depthCubeRG;
			att.clearDesc = meshClear;

			ReflectionCaptureConstants base{};
			for (int face = 0; face < 6; ++face)
			{
				const mathUtils::Mat4 vp = proj90 * FaceView(capturePos, face);
				const mathUtils::Mat4 vpT = mathUtils::Transpose(vp);
				std::memcpy(base.uFaceViewProj.data() + face * 16, mathUtils::ValuePtr(vpT), sizeof(float) * 16);
			}

			base.uCapturePosAmbient = { capturePos.x, capturePos.y, capturePos.z, 0.22f };
			base.uParams = { static_cast<float>(lightCount), 0.0f, 0.0f, 0.0f };

			graph.AddPass("ReflectionCapture_VI", std::move(att),
				[this, base, lightCount, instStride, captureMainBatches](renderGraph::PassContext& ctx) mutable
				{
					ctx.commandList.SetViewport(0, 0, (int)ctx.passExtent.width, (int)ctx.passExtent.height);
					ctx.commandList.SetState(state_);
					ctx.commandList.BindPipeline(psoReflectionCaptureVI_);
					ctx.commandList.BindStructuredBufferSRV(2, lightsBuffer_);

					for (const Batch& b : captureMainBatches)
					{
						if (!b.mesh || b.instanceCount == 0)
							continue;

						std::uint32_t flags = 0u;
						const bool useTex = (b.materialHandle.id != 0) && (b.material.albedoDescIndex != 0);
						if (useTex) flags |= 1u;

						ctx.commandList.BindTextureDesc(0, useTex ? b.material.albedoDescIndex : 0);

						ReflectionCaptureConstants c = base;
						c.uBaseColor = { b.material.baseColor.x, b.material.baseColor.y, b.material.baseColor.z, b.material.baseColor.w };
						c.uParams[1] = AsFloatBits(flags);

						ctx.commandList.BindInputLayout(b.mesh->layoutInstanced);
						ctx.commandList.BindVertexBuffer(0, b.mesh->vertexBuffer, b.mesh->vertexStrideBytes, 0);
						ctx.commandList.BindVertexBuffer(1, instanceBuffer_, instStride, b.instanceOffset * instStride);
						ctx.commandList.BindIndexBuffer(b.mesh->indexBuffer, b.mesh->indexType, 0);

						ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &c, 1 }));
						ctx.commandList.DrawIndexed(b.mesh->indexCount, b.mesh->indexType, 0, 0, b.instanceCount, 0);
					}
				});
		}
		// ---------------- Fallback path (6 passes) ----------------
		else
		{
			// Fallback depth: RenderGraph cannot select cube DSV per-face (only all-faces),
			// so we reuse the 2D temp depth texture created above (depthTmp)
			for (int face = 0; face < 6; ++face)
			{
				renderGraph::PassAttachments att{};
				att.useSwapChainBackbuffer = false;
				att.color = cubeRG;
				att.colorCubeFace = static_cast<std::uint32_t>(face);
				att.depth = depthTmp;
				att.clearDesc = meshClear;

				const mathUtils::Mat4 vp = proj90 * FaceView(capturePos, face);
				const mathUtils::Mat4 vpT = mathUtils::Transpose(vp);

				ReflectionCaptureFaceConstants base{};
				std::memcpy(base.uViewProj.data(), mathUtils::ValuePtr(vpT), sizeof(float) * 16);
				base.uCapturePosAmbient = { capturePos.x, capturePos.y, capturePos.z, 0.22f };
				base.uParams = { static_cast<float>(lightCount), 0.0f, 0.0f, 0.0f };

				const std::string passName = "ReflectionCapture_Face_" + std::to_string(face);

				graph.AddPass(passName, std::move(att),
					[this, base, lightCount, instStride, captureMainBatches](renderGraph::PassContext& ctx) mutable
					{
						ctx.commandList.SetViewport(0, 0, (int)ctx.passExtent.width, (int)ctx.passExtent.height);
						ctx.commandList.SetState(state_);
						ctx.commandList.BindPipeline(psoReflectionCapture_);
						ctx.commandList.BindStructuredBufferSRV(2, lightsBuffer_);

						for (const Batch& b : captureMainBatches)
						{
							if (!b.mesh || b.instanceCount == 0)
								continue;

							std::uint32_t flags = 0u;
							const bool useTex = (b.materialHandle.id != 0) && (b.material.albedoDescIndex != 0);
							if (useTex) flags |= 1u;

							ctx.commandList.BindTextureDesc(0, useTex ? b.material.albedoDescIndex : 0);

							ReflectionCaptureFaceConstants c = base;
							c.uBaseColor = { b.material.baseColor.x, b.material.baseColor.y, b.material.baseColor.z, b.material.baseColor.w };
							c.uParams[1] = AsFloatBits(flags);

							ctx.commandList.BindInputLayout(b.mesh->layoutInstanced);
							ctx.commandList.BindVertexBuffer(0, b.mesh->vertexBuffer, b.mesh->vertexStrideBytes, 0);
							ctx.commandList.BindVertexBuffer(1, instanceBuffer_, instStride, b.instanceOffset * instStride);
							ctx.commandList.BindIndexBuffer(b.mesh->indexBuffer, b.mesh->indexType, 0);

							ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &c, 1 }));
							ctx.commandList.DrawIndexed(b.mesh->indexCount, b.mesh->indexType, 0, 0, b.instanceCount, 0);
						}
					});
			}
		}
	}
}
*/
