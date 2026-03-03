// ---------------- ReflectionCapture pass (cubemap) ----------------
// Per-object reflection probes.
// Each reflective object gets its own probe cubemap and excludes itself from its own capture.

if (settings_.enableReflectionCapture && psoReflectionCapture_ && !reflectiveOwnerDrawItems_.empty())
{
	auto GetDrawItemWorldPos = [&scene](int drawItemIndex) -> mathUtils::Vec3
	{
		if (drawItemIndex < 0 || static_cast<std::size_t>(drawItemIndex) >= scene.drawItems.size())
		{
			return {};
		}

		const DrawItem& di = scene.drawItems[static_cast<std::size_t>(drawItemIndex)];
		if (di.transform.useMatrix)
		{
			const mathUtils::Vec4& t = di.transform.matrix[3];
			return { t.x, t.y, t.z };
		}
		return di.transform.position;
	};

	const bool canUseLayered =
		(!disableReflectionCaptureLayered_) &&
		(psoReflectionCaptureLayered_) &&
		device_.SupportsShaderModel6() &&
		device_.SupportsVPAndRTArrayIndexFromAnyShader();

	const bool canUseVI =
		(!disableReflectionCaptureVI_) &&
		(psoReflectionCaptureVI_) &&
		device_.SupportsShaderModel6() &&
		device_.SupportsViewInstancing();

	const float nearZ = std::max(0.001f, settings_.reflectionCaptureNearZ);
	const float farZ = std::max(nearZ + 0.01f, settings_.reflectionCaptureFarZ);
	const mathUtils::Mat4 proj90 = mathUtils::PerspectiveRH_ZO(mathUtils::DegToRad(90.0f), 1.0f, nearZ, farZ);

	rhi::ClearDesc clearColorDepth{};
	clearColorDepth.clearColor = true;
	clearColorDepth.clearDepth = true;
	clearColorDepth.color = { 0.0f, 0.0f, 0.0f, 1.0f };
	clearColorDepth.depth = 1.0f;

	rhi::ClearDesc clearDepthOnly{};
	clearDepthOnly.clearColor = false;
	clearDepthOnly.clearDepth = true;
	clearDepthOnly.depth = 1.0f;

	const std::uint32_t skyboxDesc = scene.skyboxDescIndex;
	const bool haveSkybox = (skyboxDesc != 0);

	for (std::size_t probeIndex = 0; probeIndex < reflectiveOwnerDrawItems_.size(); ++probeIndex)
	{
		if (probeIndex >= reflectionProbes_.size())
		{
			break;
		}

		ReflectionProbeRuntime& probe = reflectionProbes_[probeIndex];
		const int ownerDrawItem = reflectiveOwnerDrawItems_[probeIndex];

		probe.ownerDrawItem = ownerDrawItem;
		probe.capturePos = GetDrawItemWorldPos(ownerDrawItem);

		if (!probe.hasLastPos)
		{
			probe.dirty = true;
		}
		else
		{
			const mathUtils::Vec3 d = probe.capturePos - probe.lastPos;
			const float dist2 = mathUtils::Dot(d, d);
			if (dist2 > 1.0e-6f)
			{
				probe.dirty = true;
			}
		}

		if (!probe.cube || !probe.depthCube || probe.cubeDescIndex == 0)
		{
			continue;
		}

		const bool doUpdate = settings_.reflectionCaptureUpdateEveryFrame || probe.dirty;
		if (!doUpdate)
		{
			continue;
		}

		probe.dirty = false;
		probe.hasLastPos = true;
		probe.lastPos = probe.capturePos;

		const auto cubeRG = graph.ImportTexture(probe.cube, renderGraph::RGTextureDesc{
			.extent = reflectionCubeExtent_,
			.format = rhi::Format::RGBA8_UNORM,
			.usage = renderGraph::ResourceUsage::RenderTarget,
			.type = renderGraph::TextureType::Cube,
			.debugName = "ReflectionProbeCube"
		});

		const auto depthCubeRG = graph.ImportTexture(probe.depthCube, renderGraph::RGTextureDesc{
			.extent = reflectionCubeExtent_,
			.format = rhi::Format::D32_FLOAT,
			.usage = renderGraph::ResourceUsage::DepthStencil,
			.type = renderGraph::TextureType::Cube,
			.debugName = "ReflectionProbeDepthCube"
		});

		const auto depthTmp = graph.CreateTexture(renderGraph::RGTextureDesc{
			.extent = reflectionCubeExtent_,
			.format = rhi::Format::D32_FLOAT,
			.usage = renderGraph::ResourceUsage::DepthStencil,
			.debugName = "ReflectionProbeDepthTmp"
		});

		std::vector<Batch> captureMainBatches;
		captureMainBatches.reserve(captureMainBatchesNoCull.size());
		for (const Batch& b : captureMainBatchesNoCull)
		{
			if (b.reflectionProbeIndex == static_cast<int>(probeIndex))
			{
				continue;
			}
			captureMainBatches.push_back(b);
		}

		std::vector<Batch> captureReflectionBatchesLayered;
		captureReflectionBatchesLayered.reserve(reflectionBatchesLayered.size());
		for (const Batch& b : reflectionBatchesLayered)
		{
			if (b.reflectionProbeIndex == static_cast<int>(probeIndex))
			{
				continue;
			}
			captureReflectionBatchesLayered.push_back(b);
		}

		bool renderedSkybox = false;
		if (haveSkybox)
		{
			renderedSkybox = true;
			for (int face = 0; face < 6; ++face)
			{
				renderGraph::PassAttachments att{};
				att.useSwapChainBackbuffer = false;
				att.colors = { cubeRG };
				att.colorCubeFace = static_cast<std::uint32_t>(face);
				att.depth = depthTmp;
				att.clearDesc = clearColorDepth;

				mathUtils::Mat4 view = CubeFaceViewRH(probe.capturePos, face);
				view[3] = mathUtils::Vec4(0, 0, 0, 1);

				const mathUtils::Mat4 viewProjSkybox = proj90 * view;
				const mathUtils::Mat4 viewProjSkyboxT = mathUtils::Transpose(viewProjSkybox);

				SkyboxConstants skyboxConstants{};
				std::memcpy(skyboxConstants.uViewProj.data(), mathUtils::ValuePtr(viewProjSkyboxT), sizeof(float) * 16);

				const std::string passName =
					"ReflectionProbe_" + std::to_string(probeIndex) + "_Skybox_Face_" + std::to_string(face);

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

		const rhi::ClearDesc meshClear = renderedSkybox ? clearDepthOnly : clearColorDepth;

		if (canUseLayered && !captureReflectionBatchesLayered.empty())
		{
			renderGraph::PassAttachments att{};
			att.useSwapChainBackbuffer = false;
			att.colors = { cubeRG };
			att.colorCubeAllFaces = true;
			att.depth = depthCubeRG;
			att.clearDesc = meshClear;

			ReflectionCaptureConstants base{};
			for (int face = 0; face < 6; ++face)
			{
				const mathUtils::Mat4 vp = proj90 * CubeFaceViewRH(probe.capturePos, face);
				const mathUtils::Mat4 vpT = mathUtils::Transpose(vp);
				std::memcpy(base.uFaceViewProj.data() + face * 16, mathUtils::ValuePtr(vpT), sizeof(float) * 16);
			}

			base.uCapturePosAmbient = { probe.capturePos.x, probe.capturePos.y, probe.capturePos.z, 0.22f };
			base.uParams = { static_cast<float>(lightCount), 0.0f, 0.0f, 0.0f };

			const std::string passName = "ReflectionProbe_" + std::to_string(probeIndex) + "_Layered";
			graph.AddPass(passName, std::move(att),
				[this, base, instStride, captureReflectionBatchesLayered](renderGraph::PassContext& ctx) mutable
				{
					ctx.commandList.SetViewport(0, 0, (int)ctx.passExtent.width, (int)ctx.passExtent.height);
					ctx.commandList.SetState(state_);
					ctx.commandList.BindPipeline(psoReflectionCaptureLayered_);
					ctx.commandList.BindStructuredBufferSRV(2, lightsBuffer_);

					for (const Batch& b : captureReflectionBatchesLayered)
					{
						if (!b.mesh || b.instanceCount == 0)
						{
							continue;
						}

						std::uint32_t flags = 0u;
						const bool useTex = (b.materialHandle.id != 0) && (b.material.albedoDescIndex != 0);
						if (useTex)
						{
							flags |= 1u;
						}

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
		else if (canUseVI)
		{
			renderGraph::PassAttachments att{};
			att.useSwapChainBackbuffer = false;
			att.colors = { cubeRG };
			att.colorCubeAllFaces = true;
			att.depth = depthCubeRG;
			att.clearDesc = meshClear;

			ReflectionCaptureConstants base{};
			for (int face = 0; face < 6; ++face)
			{
				const mathUtils::Mat4 vp = proj90 * CubeFaceViewRH(probe.capturePos, face);
				const mathUtils::Mat4 vpT = mathUtils::Transpose(vp);
				std::memcpy(base.uFaceViewProj.data() + face * 16, mathUtils::ValuePtr(vpT), sizeof(float) * 16);
			}

			base.uCapturePosAmbient = { probe.capturePos.x, probe.capturePos.y, probe.capturePos.z, 0.22f };
			base.uParams = { static_cast<float>(lightCount), 0.0f, 0.0f, 0.0f };

			const std::string passName = "ReflectionProbe_" + std::to_string(probeIndex) + "_VI";
			graph.AddPass(passName, std::move(att),
				[this, base, instStride, captureMainBatches](renderGraph::PassContext& ctx) mutable
				{
					ctx.commandList.SetViewport(0, 0, (int)ctx.passExtent.width, (int)ctx.passExtent.height);
					ctx.commandList.SetState(state_);
					ctx.commandList.BindPipeline(psoReflectionCaptureVI_);
					ctx.commandList.BindStructuredBufferSRV(2, lightsBuffer_);

					for (const Batch& b : captureMainBatches)
					{
						if (!b.mesh || b.instanceCount == 0)
						{
							continue;
						}

						std::uint32_t flags = 0u;
						const bool useTex = (b.materialHandle.id != 0) && (b.material.albedoDescIndex != 0);
						if (useTex)
						{
							flags |= 1u;
						}

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
		else
		{
			for (int face = 0; face < 6; ++face)
			{
				renderGraph::PassAttachments att{};
				att.useSwapChainBackbuffer = false;
				att.colors = { cubeRG };
				att.colorCubeFace = static_cast<std::uint32_t>(face);
				att.depth = depthTmp;
				att.clearDesc = meshClear;

				const mathUtils::Mat4 view = CubeFaceViewRH(probe.capturePos, face);
				const mathUtils::Mat4 vp = proj90 * view;
				const mathUtils::Mat4 vpT = mathUtils::Transpose(vp);

				ReflectionCaptureFaceConstants base{};
				std::memcpy(base.uViewProj.data(), mathUtils::ValuePtr(vpT), sizeof(float) * 16);
				base.uCapturePosAmbient = { probe.capturePos.x, probe.capturePos.y, probe.capturePos.z, 0.22f };
				base.uParams = { static_cast<float>(lightCount), 0.0f, 0.0f, 0.0f };

				const std::string passName =
					"ReflectionProbe_" + std::to_string(probeIndex) + "_Face_" + std::to_string(face);

				graph.AddPass(passName, std::move(att),
					[this, base, instStride, captureMainBatches](renderGraph::PassContext& ctx) mutable
					{
						ctx.commandList.SetViewport(0, 0, (int)ctx.passExtent.width, (int)ctx.passExtent.height);
						ctx.commandList.SetState(state_);
						ctx.commandList.BindPipeline(psoReflectionCapture_);
						ctx.commandList.BindStructuredBufferSRV(2, lightsBuffer_);

						for (const Batch& b : captureMainBatches)
						{
							if (!b.mesh || b.instanceCount == 0)
							{
								continue;
							}

							std::uint32_t flags = 0u;
							const bool useTex = (b.materialHandle.id != 0) && (b.material.albedoDescIndex != 0);
							if (useTex)
							{
								flags |= 1u;
							}

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
