// ---------------- Deferred path (DX12) ----------------
const bool canDeferred =
settings_.enableDeferred &&
device_.GetBackend() == rhi::Backend::DirectX12 &&
psoDeferredGBuffer_ &&
psoDeferredLighting_ &&
fullscreenLayout_ &&
swapChain.GetDepthTexture();

if (canDeferred)
{
	// Precompute camera matrices once for deferred passes.
	const float aspect = scDesc.extent.height
		? (static_cast<float>(scDesc.extent.width) / static_cast<float>(scDesc.extent.height))
		: 1.0f;
	const mathUtils::Mat4 proj = mathUtils::PerspectiveRH_ZO(mathUtils::DegToRad(scene.camera.fovYDeg), aspect, scene.camera.nearZ, scene.camera.farZ);
	const mathUtils::Mat4 view = mathUtils::LookAt(scene.camera.position, scene.camera.target, scene.camera.up);
	const mathUtils::Mat4 viewProj = proj * view;
	const mathUtils::Mat4 invViewProj = mathUtils::Inverse(viewProj);
	const mathUtils::Mat4 invViewProjT = mathUtils::Transpose(invViewProj);
	const mathUtils::Vec3 camPosLocal = scene.camera.position;
	const mathUtils::Vec3 camFLocal = mathUtils::Normalize(scene.camera.target - scene.camera.position);

	struct alignas(16) DeferredLightingConstants
	{
		std::array<float, 16> uInvViewProj{};
		std::array<float, 4> uCameraPosAmbient{}; // xyz + ambientStrength
		std::array<float, 4> uCameraForward{}; // xyz + pad
		std::array<float, 4> uShadowBias{}; // x=dirBaseBiasTexels, y=spotBaseBiasTexels, z=pointBaseBiasTexels, w=slopeScaleTexels
		std::array<float, 4> uCounts{}; // x = lightCount, y = spotShadowCount, z = pointShadowCount
	};
	static_assert(sizeof(DeferredLightingConstants) == 128);

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
	deferredConstants.uCounts = { static_cast<float>(lightCount), static_cast<float>(spotShadows.size()), static_cast<float>(pointShadows.size()), 0.0f };

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

	// --- Editor selection (shared for deferred passes) ---
	// We keep the exact same selection logic as the forward path:
	// - Opaque selection outline/highlight is drawn BEFORE transparent objects.
	// - Transparent selection outline/highlight is drawn AFTER the transparent pass.
	constexpr std::uint32_t kEditorOutlineStencilRef = 0x80u;
	std::vector<EditorSelectionDraw> selectionOpaque;
	std::vector<EditorSelectionDraw> selectionTransparent;
	std::vector<InstanceData> selectionInstances;
	std::vector<std::uint32_t> selectionOpaqueStart;
	std::vector<std::uint32_t> selectionTransparentStart;
	{
		constexpr std::size_t kMaxSelectionInstances = 4096;
		selectionOpaque.reserve(scene.editorSelectedDrawItems.size());
		selectionTransparent.reserve(scene.editorSelectedDrawItems.size());
		selectionInstances.reserve(std::min(scene.editorSelectedDrawItems.size(), kMaxSelectionInstances));
		selectionOpaqueStart.reserve(scene.editorSelectedDrawItems.size());
		selectionTransparentStart.reserve(scene.editorSelectedDrawItems.size());

		for (const int diIndex : scene.editorSelectedDrawItems)
		{
			if (diIndex < 0)
			{
				continue;
			}
			if (selectionInstances.size() >= kMaxSelectionInstances)
			{
				break;
			}
			const std::size_t idx = static_cast<std::size_t>(diIndex);
			if (idx >= scene.drawItems.size())
			{
				continue;
			}

			const DrawItem& di = scene.drawItems[idx];
			const rendern::MeshRHI* mesh = di.mesh ? &di.mesh->GetResource() : nullptr;
			if (!mesh || mesh->indexCount == 0 || !mesh->vertexBuffer || !mesh->indexBuffer)
			{
				continue;
			}

			EditorSelectionDraw sel{};
			sel.mesh = mesh;

			const mathUtils::Mat4 model = di.transform.ToMatrix();
			sel.instance.i0 = model[0];
			sel.instance.i1 = model[1];
			sel.instance.i2 = model[2];
			sel.instance.i3 = model[3];

			sel.outlineWorldOffset = 0.01f;
			if (di.mesh)
			{
				const auto& bounds = di.mesh->GetBounds();
				if (bounds.sphereRadius > 0.0f)
				{
					sel.outlineWorldOffset = std::max(0.01f, bounds.sphereRadius * 0.03f);
				}
			}

			if (di.material.id != 0)
			{
				const auto& mat = scene.GetMaterial(di.material);
				const MaterialPerm perm = EffectivePerm(mat);
				sel.isTransparent = HasFlag(perm, MaterialPerm::Transparent);
			}
			else
			{
				sel.isTransparent = false;
			}

			const std::uint32_t startInstance = static_cast<std::uint32_t>(selectionInstances.size());
			selectionInstances.push_back(sel.instance);

			if (sel.isTransparent)
			{
				selectionTransparent.push_back(sel);
				selectionTransparentStart.push_back(startInstance);
			}
			else
			{
				selectionOpaque.push_back(sel);
				selectionOpaqueStart.push_back(startInstance);
			}
		}
	}

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
			[this, &scene, dirLightViewProj, lightCount, mainBatches, instStride, gbuf0, gbuf1, gbuf2, gbuf3](renderGraph::PassContext& ctx)
			{
				const auto extent = ctx.passExtent;

				ctx.commandList.SetViewport(0, 0,
					static_cast<int>(extent.width),
					static_cast<int>(extent.height));

				ctx.commandList.SetState(state_);
				ctx.commandList.BindPipeline(psoDeferredGBuffer_);

				const float aspect = extent.height
					? (static_cast<float>(extent.width) / static_cast<float>(extent.height))
					: 1.0f;

				const mathUtils::Mat4 proj = mathUtils::PerspectiveRH_ZO(mathUtils::DegToRad(scene.camera.fovYDeg), aspect, scene.camera.nearZ, scene.camera.farZ);
				const mathUtils::Mat4 view = mathUtils::LookAt(scene.camera.position, scene.camera.target, scene.camera.up);
				const mathUtils::Mat4 viewProj = proj * view;

				const mathUtils::Vec3 camPosLocal = scene.camera.position;
				const mathUtils::Vec3 camFLocal = mathUtils::Normalize(scene.camera.target - scene.camera.position);

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
					constants.uCounts = { static_cast<float>(lightCount), 0.0f, 0.0f, 0.0f };
					constants.uShadowBias = { 0.0f, 0.0f, 0.0f, 0.0f };
					constants.uEnvProbeBoxMin = { 0.0f, 0.0f, 0.0f, 0.0f };
					constants.uEnvProbeBoxMax = { 0.0f, 0.0f, 0.0f, 0.0f };

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

	struct SSAOConstants
	{
		std::array<float, 16> uInvViewProj{};
		mathUtils::Vec4 uParams{};  // radius, bias, strength, power
		mathUtils::Vec4 uInvSize{}; // 1/w, 1/h, 0,0
	};
	static_assert(sizeof(SSAOConstants) % 16 == 0);

	struct SSAOBlurConstants
	{
		mathUtils::Vec4 uInvSize{};
		mathUtils::Vec4 uParams{}; // depthThreshold
	};
	static_assert(sizeof(SSAOBlurConstants) % 16 == 0);

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

				const float aspect = extent.height
					? (static_cast<float>(extent.width) / static_cast<float>(extent.height))
					: 1.0f;
				const mathUtils::Mat4 proj = mathUtils::PerspectiveRH_ZO(mathUtils::DegToRad(scene.camera.fovYDeg), aspect, scene.camera.nearZ, scene.camera.farZ);
				const mathUtils::Mat4 view = mathUtils::LookAt(scene.camera.position, scene.camera.target, scene.camera.up);
				const mathUtils::Mat4 viewProj = proj * view;
				const mathUtils::Mat4 invViewProj = mathUtils::Inverse(viewProj);
				const mathUtils::Mat4 invViewProjT = mathUtils::Transpose(invViewProj);

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
			[this, &scene, gbuf0, gbuf1, gbuf2, gbuf3, depthRG, shadowRG, spotShadows, pointShadows, deferredConstants, ssaoBlur](renderGraph::PassContext& ctx)
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
				ctx.commandList.BindTextureDesc(17, reflectionCubeDescIndex_);

				// Lights (t16) and SSAO (t18)
				ctx.commandList.BindStructuredBufferSRV(16, lightsBuffer_);
				ctx.commandList.BindTexture2D(18, ctx.resources.GetTexture(ssaoBlur));

				ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &deferredConstants, 1 }));
				ctx.commandList.Draw(3);
			});
	}

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
			[this, &scene, instStride](renderGraph::PassContext& ctx)
			{
				const auto extent = ctx.passExtent;

				const float aspect = extent.height
					? (static_cast<float>(extent.width) / static_cast<float>(extent.height))
					: 1.0f;

				const mathUtils::Mat4 proj = mathUtils::PerspectiveRH_ZO(mathUtils::DegToRad(scene.camera.fovYDeg), aspect, scene.camera.nearZ, scene.camera.farZ);
				const mathUtils::Mat4 view = mathUtils::LookAt(scene.camera.position, scene.camera.target, scene.camera.up);

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
			.format = rhi::Format::RGBA8_UNORM,
			.usage = renderGraph::ResourceUsage::RenderTarget,
			.debugName = "SceneColor_Fog"
			});

		struct alignas(16) FogConstants
		{
			std::array<float, 16> uInvViewProj{};
			std::array<float, 4> uCameraPos{}; // xyz + pad
			std::array<float, 4> uFogParams{}; // start, end, density, mode
			std::array<float, 4> uFogColor{};  // rgb + enabled(0/1)
		};
		static_assert(sizeof(FogConstants) % 16 == 0);

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
			instStride](renderGraph::PassContext& ctx)
			{
				const auto extent = ctx.passExtent;
				ctx.commandList.SetViewport(0, 0,
					static_cast<int>(extent.width),
					static_cast<int>(extent.height));

				const float aspect = extent.height
					? (static_cast<float>(extent.width) / static_cast<float>(extent.height))
					: 1.0f;
				const mathUtils::Mat4 proj = mathUtils::PerspectiveRH_ZO(mathUtils::DegToRad(scene.camera.fovYDeg), aspect, scene.camera.nearZ, scene.camera.farZ);
				const mathUtils::Mat4 view = mathUtils::LookAt(scene.camera.position, scene.camera.target, scene.camera.up);
				const mathUtils::Mat4 viewProj = proj * view;
				const mathUtils::Vec3 camPosLocal = scene.camera.position;
				const mathUtils::Vec3 camFLocal = mathUtils::Normalize(scene.camera.target - scene.camera.position);

				// Upload all selection instances at once (DX12 UpdateBuffer is deferred; do it once per pass).
				if (!selectionInstances.empty() && highlightInstanceBuffer_)
				{
					device_.UpdateBuffer(highlightInstanceBuffer_, std::as_bytes(std::span{ selectionInstances }));
				}

				auto BindEditorSelectionGeometry = [&](const rendern::MeshRHI& mesh)
					{
						ctx.commandList.BindInputLayout(mesh.layoutInstanced);
						ctx.commandList.BindVertexBuffer(0, mesh.vertexBuffer, mesh.vertexStrideBytes, 0);
						ctx.commandList.BindVertexBuffer(1, highlightInstanceBuffer_, instStride, 0);
						ctx.commandList.BindIndexBuffer(mesh.indexBuffer, mesh.indexType, 0);
					};

				auto DrawEditorSelectionHighlight = [&](const rendern::MeshRHI& mesh, std::uint32_t startInstance)
					{
						if (!highlightInstanceBuffer_)
						{
							return;
						}

						ctx.commandList.SetState(highlightState_);
						ctx.commandList.BindPipeline(psoHighlight_);

						PerBatchConstants constants{};
						const mathUtils::Mat4 viewProjT = mathUtils::Transpose(viewProj);
						const mathUtils::Mat4 dirVP_T = mathUtils::Transpose(dirLightViewProj);
						std::memcpy(constants.uViewProj.data(), mathUtils::ValuePtr(viewProjT), sizeof(float) * 16);
						std::memcpy(constants.uLightViewProj.data(), mathUtils::ValuePtr(dirVP_T), sizeof(float) * 16);
						constants.uCameraAmbient = { camPosLocal.x, camPosLocal.y, camPosLocal.z, 0.0f };
						constants.uCameraForward = { camFLocal.x, camFLocal.y, camFLocal.z, 0.0f };
						constants.uBaseColor = { 1.0f, 0.86f, 0.10f, 0.22f };
						constants.uMaterialFlags = { 0.0f, 0.0f, 0.0f, AsFloatBits(0u) };
						constants.uPbrParams = { 0.0f, 1.0f, 1.0f, 0.0f };
						constants.uCounts = { 0.0f, 0.0f, 0.0f, 0.0f };
						constants.uShadowBias = { 0.0f, 0.0f, 0.0f, 0.0f };
						constants.uEnvProbeBoxMin = { 0.0f, 0.0f, 0.0f, 0.0f };
						constants.uEnvProbeBoxMax = { 0.0f, 0.0f, 0.0f, 0.0f };
						ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &constants, 1 }));
						ctx.commandList.DrawIndexed(mesh.indexCount, mesh.indexType, 0, 0, 1, startInstance);
					};

				auto DrawEditorSelectionOutline = [&](const rendern::MeshRHI& mesh, float editorOutlineWorldOffset, std::uint32_t startInstance)
					{
						if (!highlightInstanceBuffer_ || !psoOutline_)
						{
							return;
						}

						const mathUtils::Mat4 viewProjT = mathUtils::Transpose(viewProj);
						const mathUtils::Mat4 dirVP_T = mathUtils::Transpose(dirLightViewProj);
						BindEditorSelectionGeometry(mesh);

						// 1) Mark visible selected-object pixels in stencil.
						ctx.commandList.SetStencilRef(kEditorOutlineStencilRef);
						ctx.commandList.SetState(outlineMarkState_);
						ctx.commandList.BindPipeline(psoHighlight_);

						PerBatchConstants markConstants{};
						std::memcpy(markConstants.uViewProj.data(), mathUtils::ValuePtr(viewProjT), sizeof(float) * 16);
						std::memcpy(markConstants.uLightViewProj.data(), mathUtils::ValuePtr(dirVP_T), sizeof(float) * 16);
						markConstants.uCameraAmbient = { camPosLocal.x, camPosLocal.y, camPosLocal.z, 0.0f };
						markConstants.uCameraForward = { camFLocal.x, camFLocal.y, camFLocal.z, 0.0f };
						markConstants.uBaseColor = { 1.0f, 1.0f, 1.0f, 0.0f };
						markConstants.uMaterialFlags = { 0.0f, 0.0f, 0.0f, AsFloatBits(0u) };
						markConstants.uPbrParams = { 0.0f, 1.0f, 1.0f, 0.0f };
						markConstants.uCounts = { 0.0f, 0.0f, 0.0f, 0.0f };
						markConstants.uShadowBias = { 0.0f, 0.0f, 0.0f, 0.0f };
						markConstants.uEnvProbeBoxMin = { 0.0f, 0.0f, 0.0f, 0.0f };
						markConstants.uEnvProbeBoxMax = { 0.0f, 0.0f, 0.0f, 0.0f };
						ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &markConstants, 1 }));
						ctx.commandList.DrawIndexed(mesh.indexCount, mesh.indexType, 0, 0, 1, startInstance);

						// 2) Draw inflated mesh only outside the marked silhouette.
						ctx.commandList.SetState(outlineState_);
						ctx.commandList.BindPipeline(psoOutline_);

						PerBatchConstants outlineConstants{};
						std::memcpy(outlineConstants.uViewProj.data(), mathUtils::ValuePtr(viewProjT), sizeof(float) * 16);
						std::memcpy(outlineConstants.uLightViewProj.data(), mathUtils::ValuePtr(dirVP_T), sizeof(float) * 16);
						outlineConstants.uCameraAmbient = { camPosLocal.x, camPosLocal.y, camPosLocal.z, 0.0f };
						outlineConstants.uCameraForward = { camFLocal.x, camFLocal.y, camFLocal.z, 0.0f };
						outlineConstants.uBaseColor = { 1.0f, 0.72f, 0.10f, 0.95f };
						outlineConstants.uMaterialFlags = { 0.0f, 0.0f, 0.0f, AsFloatBits(0u) };
						outlineConstants.uPbrParams = { editorOutlineWorldOffset, 0.0f, 0.0f, 0.0f };
						outlineConstants.uCounts = { 0.0f, 0.0f, 0.0f, 3.0f };
						outlineConstants.uShadowBias = {
							extent.width ? (1.0f / static_cast<float>(extent.width)) : 0.0f,
							extent.height ? (1.0f / static_cast<float>(extent.height)) : 0.0f,
							0.0f,
							0.0f
						};
						outlineConstants.uEnvProbeBoxMin = { 0.0f, 0.0f, 0.0f, 0.0f };
						outlineConstants.uEnvProbeBoxMax = { 0.0f, 0.0f, 0.0f, 0.0f };
						ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &outlineConstants, 1 }));
						ctx.commandList.DrawIndexed(mesh.indexCount, mesh.indexType, 0, 0, 1, startInstance);

						// 3) Clear the stencil mark for this object so outlines for multiple selected objects don't interfere.
						ctx.commandList.SetState(outlineMarkState_);
						ctx.commandList.BindPipeline(psoHighlight_);
						ctx.commandList.SetStencilRef(0u);
						ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &markConstants, 1 }));
						ctx.commandList.DrawIndexed(mesh.indexCount, mesh.indexType, 0, 0, 1, startInstance);

						ctx.commandList.SetStencilRef(0u);
						ctx.commandList.SetState(state_);
					};

				auto DrawSelectionGroup = [&](const std::vector<EditorSelectionDraw>& group, const std::vector<std::uint32_t>& starts)
					{
						const std::size_t count = std::min(group.size(), starts.size());
						for (std::size_t i = 0; i < count; ++i)
						{
							const EditorSelectionDraw& s = group[i];
							if (!s.mesh)
							{
								continue;
							}
							const std::uint32_t startInstance = starts[i];
							DrawEditorSelectionOutline(*s.mesh, s.outlineWorldOffset, startInstance);
							DrawEditorSelectionHighlight(*s.mesh, startInstance);
						}
					};

				DrawSelectionGroup(selectionOpaque, selectionOpaqueStart);
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

				constexpr std::uint32_t kFlagUseTex = 1u << 0;
				constexpr std::uint32_t kFlagUseShadow = 1u << 1;
				constexpr std::uint32_t kFlagUseNormal = 1u << 2;
				constexpr std::uint32_t kFlagUseMetalTex = 1u << 3;
				constexpr std::uint32_t kFlagUseRoughTex = 1u << 4;
				constexpr std::uint32_t kFlagUseAOTex = 1u << 5;
				constexpr std::uint32_t kFlagUseEmissiveTex = 1u << 6;
				constexpr std::uint32_t kFlagUseEnv = 1u << 7;
				constexpr std::uint32_t kFlagEnvForceMip0 = 1u << 8;
				constexpr std::uint32_t kFlagEnvFlipZ = 1u << 9;

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

					bool usingReflectionProbeEnv = false;
					rhi::TextureHandle envArrayTexture{};
					rhi::TextureDescIndex envDescIndex = scene.skyboxDescIndex;
					if (batchTransparent.materialHandle.id != 0)
					{
						const auto& mat = scene.GetMaterial(batchTransparent.materialHandle);

						if (mat.envSource == EnvSource::ReflectionCapture && settings_.enableReflectionCapture)
						{
							if (reflectionCubeDescIndex_ != 0 && reflectionCube_)
							{
								envDescIndex = reflectionCubeDescIndex_;
								envArrayTexture = reflectionCube_;
								usingReflectionProbeEnv = true;
							}
						}
					}
					ctx.commandList.BindTextureDesc(17, envDescIndex);

					if (usingReflectionProbeEnv && envArrayTexture)
					{
						ctx.commandList.BindTexture2DArray(18, envArrayTexture);
					}

					std::uint32_t flags = 0;
					if (useTex) { flags |= kFlagUseTex; }
					if (useShadow) { flags |= kFlagUseShadow; }
					if (batchTransparent.material.normalDescIndex != 0) { flags |= kFlagUseNormal; }
					if (batchTransparent.material.metalnessDescIndex != 0) { flags |= kFlagUseMetalTex; }
					if (batchTransparent.material.roughnessDescIndex != 0) { flags |= kFlagUseRoughTex; }
					if (batchTransparent.material.aoDescIndex != 0) { flags |= kFlagUseAOTex; }
					if (batchTransparent.material.emissiveDescIndex != 0) { flags |= kFlagUseEmissiveTex; }

					if (envDescIndex != 0)
					{
						flags |= kFlagUseEnv;
						if (settings_.enableReflectionCapture && usingReflectionProbeEnv)
						{
							flags |= kFlagEnvForceMip0;
							flags |= kFlagEnvFlipZ;
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
					constants.uEnvProbeBoxMin = { 0.0f, 0.0f, 0.0f, 0.0f };
					constants.uEnvProbeBoxMax = { 0.0f, 0.0f, 0.0f, 0.0f };

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
			instStride](renderGraph::PassContext& ctx)
			{
				// Reuse the same implementation as DeferredSelectionOpaque by just delegating through the
				// exact same code path (outline+highlight), but after the transparent pass.
				// NOTE: we keep this as a separate pass so ordering stays correct.
				const auto extent = ctx.passExtent;
				ctx.commandList.SetViewport(0, 0,
					static_cast<int>(extent.width),
					static_cast<int>(extent.height));

				const float aspect = extent.height
					? (static_cast<float>(extent.width) / static_cast<float>(extent.height))
					: 1.0f;
				const mathUtils::Mat4 proj = mathUtils::PerspectiveRH_ZO(mathUtils::DegToRad(scene.camera.fovYDeg), aspect, scene.camera.nearZ, scene.camera.farZ);
				const mathUtils::Mat4 view = mathUtils::LookAt(scene.camera.position, scene.camera.target, scene.camera.up);
				const mathUtils::Mat4 viewProj = proj * view;
				const mathUtils::Vec3 camPosLocal = scene.camera.position;
				const mathUtils::Vec3 camFLocal = mathUtils::Normalize(scene.camera.target - scene.camera.position);

				if (!selectionInstances.empty() && highlightInstanceBuffer_)
				{
					device_.UpdateBuffer(highlightInstanceBuffer_, std::as_bytes(std::span{ selectionInstances }));
				}

				auto BindEditorSelectionGeometry = [&](const rendern::MeshRHI& mesh)
					{
						ctx.commandList.BindInputLayout(mesh.layoutInstanced);
						ctx.commandList.BindVertexBuffer(0, mesh.vertexBuffer, mesh.vertexStrideBytes, 0);
						ctx.commandList.BindVertexBuffer(1, highlightInstanceBuffer_, instStride, 0);
						ctx.commandList.BindIndexBuffer(mesh.indexBuffer, mesh.indexType, 0);
					};

				auto DrawEditorSelectionHighlight = [&](const rendern::MeshRHI& mesh, std::uint32_t startInstance)
					{
						if (!highlightInstanceBuffer_)
						{
							return;
						}
						ctx.commandList.SetState(highlightState_);
						ctx.commandList.BindPipeline(psoHighlight_);

						PerBatchConstants constants{};
						const mathUtils::Mat4 viewProjT = mathUtils::Transpose(viewProj);
						const mathUtils::Mat4 dirVP_T = mathUtils::Transpose(dirLightViewProj);
						std::memcpy(constants.uViewProj.data(), mathUtils::ValuePtr(viewProjT), sizeof(float) * 16);
						std::memcpy(constants.uLightViewProj.data(), mathUtils::ValuePtr(dirVP_T), sizeof(float) * 16);
						constants.uCameraAmbient = { camPosLocal.x, camPosLocal.y, camPosLocal.z, 0.0f };
						constants.uCameraForward = { camFLocal.x, camFLocal.y, camFLocal.z, 0.0f };
						constants.uBaseColor = { 1.0f, 0.86f, 0.10f, 0.22f };
						constants.uMaterialFlags = { 0.0f, 0.0f, 0.0f, AsFloatBits(0u) };
						constants.uPbrParams = { 0.0f, 1.0f, 1.0f, 0.0f };
						constants.uCounts = { 0.0f, 0.0f, 0.0f, 0.0f };
						constants.uShadowBias = { 0.0f, 0.0f, 0.0f, 0.0f };
						constants.uEnvProbeBoxMin = { 0.0f, 0.0f, 0.0f, 0.0f };
						constants.uEnvProbeBoxMax = { 0.0f, 0.0f, 0.0f, 0.0f };
						ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &constants, 1 }));
						ctx.commandList.DrawIndexed(mesh.indexCount, mesh.indexType, 0, 0, 1, startInstance);
					};

				auto DrawEditorSelectionOutline = [&](const rendern::MeshRHI& mesh, float editorOutlineWorldOffset, std::uint32_t startInstance)
					{
						if (!highlightInstanceBuffer_ || !psoOutline_)
						{
							return;
						}
						const mathUtils::Mat4 viewProjT = mathUtils::Transpose(viewProj);
						const mathUtils::Mat4 dirVP_T = mathUtils::Transpose(dirLightViewProj);
						BindEditorSelectionGeometry(mesh);

						ctx.commandList.SetStencilRef(kEditorOutlineStencilRef);
						ctx.commandList.SetState(outlineMarkState_);
						ctx.commandList.BindPipeline(psoHighlight_);

						PerBatchConstants markConstants{};
						std::memcpy(markConstants.uViewProj.data(), mathUtils::ValuePtr(viewProjT), sizeof(float) * 16);
						std::memcpy(markConstants.uLightViewProj.data(), mathUtils::ValuePtr(dirVP_T), sizeof(float) * 16);
						markConstants.uCameraAmbient = { camPosLocal.x, camPosLocal.y, camPosLocal.z, 0.0f };
						markConstants.uCameraForward = { camFLocal.x, camFLocal.y, camFLocal.z, 0.0f };
						markConstants.uBaseColor = { 1.0f, 1.0f, 1.0f, 0.0f };
						markConstants.uMaterialFlags = { 0.0f, 0.0f, 0.0f, AsFloatBits(0u) };
						markConstants.uPbrParams = { 0.0f, 1.0f, 1.0f, 0.0f };
						markConstants.uCounts = { 0.0f, 0.0f, 0.0f, 0.0f };
						markConstants.uShadowBias = { 0.0f, 0.0f, 0.0f, 0.0f };
						markConstants.uEnvProbeBoxMin = { 0.0f, 0.0f, 0.0f, 0.0f };
						markConstants.uEnvProbeBoxMax = { 0.0f, 0.0f, 0.0f, 0.0f };
						ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &markConstants, 1 }));
						ctx.commandList.DrawIndexed(mesh.indexCount, mesh.indexType, 0, 0, 1, startInstance);

						ctx.commandList.SetState(outlineState_);
						ctx.commandList.BindPipeline(psoOutline_);

						PerBatchConstants outlineConstants{};
						std::memcpy(outlineConstants.uViewProj.data(), mathUtils::ValuePtr(viewProjT), sizeof(float) * 16);
						std::memcpy(outlineConstants.uLightViewProj.data(), mathUtils::ValuePtr(dirVP_T), sizeof(float) * 16);
						outlineConstants.uCameraAmbient = { camPosLocal.x, camPosLocal.y, camPosLocal.z, 0.0f };
						outlineConstants.uCameraForward = { camFLocal.x, camFLocal.y, camFLocal.z, 0.0f };
						outlineConstants.uBaseColor = { 1.0f, 0.72f, 0.10f, 0.95f };
						outlineConstants.uMaterialFlags = { 0.0f, 0.0f, 0.0f, AsFloatBits(0u) };
						outlineConstants.uPbrParams = { editorOutlineWorldOffset, 0.0f, 0.0f, 0.0f };
						outlineConstants.uCounts = { 0.0f, 0.0f, 0.0f, 3.0f };
						outlineConstants.uShadowBias = {
							extent.width ? (1.0f / static_cast<float>(extent.width)) : 0.0f,
							extent.height ? (1.0f / static_cast<float>(extent.height)) : 0.0f,
							0.0f,
							0.0f
						};
						outlineConstants.uEnvProbeBoxMin = { 0.0f, 0.0f, 0.0f, 0.0f };
						outlineConstants.uEnvProbeBoxMax = { 0.0f, 0.0f, 0.0f, 0.0f };
						ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &outlineConstants, 1 }));
						ctx.commandList.DrawIndexed(mesh.indexCount, mesh.indexType, 0, 0, 1, startInstance);

						ctx.commandList.SetState(outlineMarkState_);
						ctx.commandList.BindPipeline(psoHighlight_);
						ctx.commandList.SetStencilRef(0u);
						ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &markConstants, 1 }));
						ctx.commandList.DrawIndexed(mesh.indexCount, mesh.indexType, 0, 0, 1, startInstance);

						ctx.commandList.SetStencilRef(0u);
						ctx.commandList.SetState(state_);
					};

				const std::size_t count = std::min(selectionTransparent.size(), selectionTransparentStart.size());
				for (std::size_t i = 0; i < count; ++i)
				{
					const EditorSelectionDraw& s = selectionTransparent[i];
					if (!s.mesh)
					{
						continue;
					}
					const std::uint32_t startInstance = selectionTransparentStart[i];
					DrawEditorSelectionOutline(*s.mesh, s.outlineWorldOffset, startInstance);
					DrawEditorSelectionHighlight(*s.mesh, startInstance);
				}
			});
	}
	// --- Present: copy SceneColor to swapchain ---
	{
		rhi::ClearDesc clear{};
		clear.clearColor = false;
		clear.clearDepth = false;
		clear.clearStencil = false;

		graph.AddSwapChainPass("DeferredPresent", clear,
			[this, sceneColorAfterFog](renderGraph::PassContext& ctx)
			{
				const auto extent = ctx.passExtent;

				ctx.commandList.SetViewport(0, 0,
					static_cast<int>(extent.width),
					static_cast<int>(extent.height));

				ctx.commandList.SetState(copyToSwapChainState_);
				ctx.commandList.BindInputLayout(fullscreenLayout_);
				ctx.commandList.BindPipeline(psoCopyToSwapChain_);

				// t0: SceneColor_Lit				
				ctx.commandList.BindTexture2D(0, ctx.resources.GetTexture(sceneColorAfterFog));
				ctx.commandList.Draw(3, 0);
			});

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
else
{
	// ---------------- Forward path via offscreen SceneColor ----------------
	const auto depthRG = graph.ImportTexture(
		swapChain.GetDepthTexture(),
		renderGraph::RGTextureDesc{
			.extent = scDesc.extent,
			.format = rhi::Format::D24_UNORM_S8_UINT,
			.usage = renderGraph::ResourceUsage::DepthStencil,
			.debugName = "SwapChainDepth_Imported_Forward"
		});

	const auto forwardSceneColor = graph.CreateTexture(renderGraph::RGTextureDesc{
		.extent = scDesc.extent,
		.format = rhi::Format::RGBA8_UNORM,
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

		struct SSAOConstants
		{
			std::array<float, 16> uInvViewProj{};
			mathUtils::Vec4 uParams{};
			mathUtils::Vec4 uInvSize{};
		};
		static_assert(sizeof(SSAOConstants) % 16 == 0);

		struct SSAOBlurConstants
		{
			mathUtils::Vec4 uInvSize{};
			mathUtils::Vec4 uParams{};
		};
		static_assert(sizeof(SSAOBlurConstants) % 16 == 0);

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

				const float aspect = extent.height
					? (static_cast<float>(extent.width) / static_cast<float>(extent.height))
					: 1.0f;

				const mathUtils::Mat4 proj = mathUtils::PerspectiveRH_ZO(
					mathUtils::DegToRad(scene.camera.fovYDeg),
					aspect,
					scene.camera.nearZ,
					scene.camera.farZ);
				const mathUtils::Mat4 view = mathUtils::LookAt(
					scene.camera.position,
					scene.camera.target,
					scene.camera.up);
				const mathUtils::Mat4 viewProj = proj * view;
				const mathUtils::Mat4 invViewProj = mathUtils::Inverse(viewProj);
				const mathUtils::Mat4 invViewProjT = mathUtils::Transpose(invViewProj);

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

	graph.AddPass("ForwardMainPass", std::move(mainAtt), [
		this,
		&scene,
		shadowRG,
		dirLightViewProj,
		lightCount,
		spotShadows,
		pointShadows,
		mainBatches,
		captureMainBatchesNoCull,
		instStride,
		transparentDraws,
		planarMirrorDraws,
		doDepthPrepass](renderGraph::PassContext& ctx)
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

		constexpr std::uint32_t kEditorOutlineStencilRef = 0x80u;

		std::vector<EditorSelectionDraw> selectionOpaque;
		std::vector<EditorSelectionDraw> selectionTransparent;

		std::vector<InstanceData> selectionInstances;
		std::vector<std::uint32_t> selectionOpaqueStart;
		std::vector<std::uint32_t> selectionTransparentStart;

		auto BindEditorSelectionGeometry = [&](const rendern::MeshRHI& mesh)
			{
				ctx.commandList.BindInputLayout(mesh.layoutInstanced);
				ctx.commandList.BindVertexBuffer(0, mesh.vertexBuffer, mesh.vertexStrideBytes, 0);
				ctx.commandList.BindVertexBuffer(1, highlightInstanceBuffer_, instStride, 0);
				ctx.commandList.BindIndexBuffer(mesh.indexBuffer, mesh.indexType, 0);
			};

		auto DrawEditorSelectionHighlight = [&](const rendern::MeshRHI& mesh, std::uint32_t startInstance)
			{
				if (!highlightInstanceBuffer_)
				{
					return;
				}

				ctx.commandList.SetState(highlightState_);
				ctx.commandList.BindPipeline(psoHighlight_);

				PerBatchConstants constants{};
				const mathUtils::Mat4 viewProjT = mathUtils::Transpose(viewProj);
				const mathUtils::Mat4 dirVP_T = mathUtils::Transpose(dirLightViewProj);
				std::memcpy(constants.uViewProj.data(), mathUtils::ValuePtr(viewProjT), sizeof(float) * 16);
				std::memcpy(constants.uLightViewProj.data(), mathUtils::ValuePtr(dirVP_T), sizeof(float) * 16);

				constants.uCameraAmbient = { camPosLocal.x, camPosLocal.y, camPosLocal.z, 0.0f };
				constants.uCameraForward = { camFLocal.x, camFLocal.y, camFLocal.z, 0.0f };
				// Slightly transparent yellow overlay.
				constants.uBaseColor = { 1.0f, 0.95f, 0.25f, 0.35f };
				constants.uMaterialFlags = { 0.0f, 0.0f, 0.0f, AsFloatBits(0u) };
				constants.uPbrParams = { 0.0f, 1.0f, 1.0f, 0.0f };
				constants.uCounts = { 0.0f, 0.0f, 0.0f, 0.0f };
				constants.uShadowBias = { 0.0f, 0.0f, 0.0f, 0.0f };
				constants.uEnvProbeBoxMin = { 0.0f, 0.0f, 0.0f, 0.0f };
				constants.uEnvProbeBoxMax = { 0.0f, 0.0f, 0.0f, 0.0f };

				BindEditorSelectionGeometry(mesh);

				ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &constants, 1 }));
				ctx.commandList.DrawIndexed(
					mesh.indexCount,
					mesh.indexType,
					0,
					0,
					1,
					startInstance);

				// Restore the main pass state (keeps following draws unchanged).
				ctx.commandList.SetState(doDepthPrepass ? mainAfterPreDepthState_ : state_);
			};

		auto DrawEditorSelectionOutline = [&](const rendern::MeshRHI& mesh, float editorOutlineWorldOffset, std::uint32_t startInstance)
			{
				if (!highlightInstanceBuffer_ || !psoOutline_)
				{
					return;
				}

				const mathUtils::Mat4 viewProjT = mathUtils::Transpose(viewProj);
				const mathUtils::Mat4 dirVP_T = mathUtils::Transpose(dirLightViewProj);

				BindEditorSelectionGeometry(mesh);

				// 1) Mark visible selected-object pixels in stencil.
				ctx.commandList.SetStencilRef(kEditorOutlineStencilRef);
				ctx.commandList.SetState(outlineMarkState_);
				ctx.commandList.BindPipeline(psoHighlight_);

				PerBatchConstants markConstants{};
				std::memcpy(markConstants.uViewProj.data(), mathUtils::ValuePtr(viewProjT), sizeof(float) * 16);
				std::memcpy(markConstants.uLightViewProj.data(), mathUtils::ValuePtr(dirVP_T), sizeof(float) * 16);
				markConstants.uCameraAmbient = { camPosLocal.x, camPosLocal.y, camPosLocal.z, 0.0f };
				markConstants.uCameraForward = { camFLocal.x, camFLocal.y, camFLocal.z, 0.0f };
				markConstants.uBaseColor = { 1.0f, 1.0f, 1.0f, 0.0f };
				markConstants.uMaterialFlags = { 0.0f, 0.0f, 0.0f, AsFloatBits(0u) };
				markConstants.uPbrParams = { 0.0f, 1.0f, 1.0f, 0.0f };
				markConstants.uCounts = { 0.0f, 0.0f, 0.0f, 0.0f };
				markConstants.uShadowBias = { 0.0f, 0.0f, 0.0f, 0.0f };
				markConstants.uEnvProbeBoxMin = { 0.0f, 0.0f, 0.0f, 0.0f };
				markConstants.uEnvProbeBoxMax = { 0.0f, 0.0f, 0.0f, 0.0f };
				ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &markConstants, 1 }));
				ctx.commandList.DrawIndexed(
					mesh.indexCount,
					mesh.indexType,
					0,
					0,
					1,
					startInstance);

				// 2) Draw inflated mesh only outside the marked silhouette.
				ctx.commandList.SetState(outlineState_);
				ctx.commandList.BindPipeline(psoOutline_);

				PerBatchConstants outlineConstants{};
				std::memcpy(outlineConstants.uViewProj.data(), mathUtils::ValuePtr(viewProjT), sizeof(float) * 16);
				std::memcpy(outlineConstants.uLightViewProj.data(), mathUtils::ValuePtr(dirVP_T), sizeof(float) * 16);
				outlineConstants.uCameraAmbient = { camPosLocal.x, camPosLocal.y, camPosLocal.z, 0.0f };
				outlineConstants.uCameraForward = { camFLocal.x, camFLocal.y, camFLocal.z, 0.0f };
				outlineConstants.uBaseColor = { 1.0f, 0.72f, 0.10f, 0.95f };
				outlineConstants.uMaterialFlags = { 0.0f, 0.0f, 0.0f, AsFloatBits(0u) };
				// x = small world-space normal probe distance used to derive a screen-space silhouette direction.
				outlineConstants.uPbrParams = { editorOutlineWorldOffset, 0.0f, 0.0f, 0.0f };
				// w = desired outline thickness in pixels.
				outlineConstants.uCounts = { 0.0f, 0.0f, 0.0f, 3.0f };
				// x/y = inverse viewport size (used only by CORE_OUTLINE path).
				outlineConstants.uShadowBias = {
					extent.width ? (1.0f / static_cast<float>(extent.width)) : 0.0f,
					extent.height ? (1.0f / static_cast<float>(extent.height)) : 0.0f,
					0.0f,
					0.0f
				};
				outlineConstants.uEnvProbeBoxMin = { 0.0f, 0.0f, 0.0f, 0.0f };
				outlineConstants.uEnvProbeBoxMax = { 0.0f, 0.0f, 0.0f, 0.0f };
				ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &outlineConstants, 1 }));
				ctx.commandList.DrawIndexed(
					mesh.indexCount,
					mesh.indexType,
					0,
					0,
					1,
					startInstance);

				// 3) Clear the stencil mark for this object so outlines for multiple selected objects
				// don't interfere with each other (otherwise the first object "occupies" the stencil
				// and later outlines get clipped).
				//
				// We reuse the same pipeline/state as the mark pass, but set stencil ref to 0.
				ctx.commandList.SetState(outlineMarkState_);
				ctx.commandList.BindPipeline(psoHighlight_);
				ctx.commandList.SetStencilRef(0u);
				ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &markConstants, 1 }));
				ctx.commandList.DrawIndexed(
					mesh.indexCount,
					mesh.indexType,
					0,
					0,
					1,
					startInstance);

				ctx.commandList.SetStencilRef(0u);
				ctx.commandList.SetState(doDepthPrepass ? mainAfterPreDepthState_ : state_);
			};

		auto UploadSelectionInstance = [&](const InstanceData& inst)
			{
				if (!highlightInstanceBuffer_)
				{
					return;
				}
				device_.UpdateBuffer(highlightInstanceBuffer_, std::as_bytes(std::span{ &inst, 1 }));
			};

		auto DrawSelectionGroup = [&](const std::vector<EditorSelectionDraw>& group, const std::vector<std::uint32_t>& starts)
			{
				const std::size_t count = std::min(group.size(), starts.size());
				for (std::size_t i = 0; i < count; ++i)
				{
					const EditorSelectionDraw& s = group[i];
					if (!s.mesh)
					{
						continue;
					}
					const std::uint32_t startInstance = starts[i];
					DrawEditorSelectionOutline(*s.mesh, s.outlineWorldOffset, startInstance);
					DrawEditorSelectionHighlight(*s.mesh, startInstance);
				}
			};

		// Build editor selection draw lists once per frame. We split into opaque/transparent selections
		// to preserve the old behavior:
		// - opaque selection outline/highlight is drawn BEFORE transparent objects,
		// - transparent selection outline/highlight is drawn AFTER the transparent pass.
		selectionOpaque.clear();
		selectionTransparent.clear();
		selectionInstances.clear();
		selectionOpaqueStart.clear();
		selectionTransparentStart.clear();

		constexpr std::size_t kMaxSelectionInstances = 4096;
		selectionOpaque.reserve(scene.editorSelectedDrawItems.size());
		selectionTransparent.reserve(scene.editorSelectedDrawItems.size());
		selectionInstances.reserve(std::min(scene.editorSelectedDrawItems.size(), kMaxSelectionInstances));
		selectionOpaqueStart.reserve(scene.editorSelectedDrawItems.size());
		selectionTransparentStart.reserve(scene.editorSelectedDrawItems.size());

		for (const int diIndex : scene.editorSelectedDrawItems)
		{
			if (diIndex < 0)
			{
				continue;
			}

			if (selectionInstances.size() >= kMaxSelectionInstances)
			{
				break;
			}

			const std::size_t idx = static_cast<std::size_t>(diIndex);
			if (idx >= scene.drawItems.size())
			{
				continue;
			}

			const DrawItem& di = scene.drawItems[idx];
			const rendern::MeshRHI* mesh = di.mesh ? &di.mesh->GetResource() : nullptr;
			if (!mesh || mesh->indexCount == 0 || !mesh->vertexBuffer || !mesh->indexBuffer)
			{
				continue;
			}

			EditorSelectionDraw sel{};
			sel.mesh = mesh;

			const mathUtils::Mat4 model = di.transform.ToMatrix();
			sel.instance.i0 = model[0];
			sel.instance.i1 = model[1];
			sel.instance.i2 = model[2];
			sel.instance.i3 = model[3];

			sel.outlineWorldOffset = 0.01f;
			if (di.mesh)
			{
				const auto& bounds = di.mesh->GetBounds();
				if (bounds.sphereRadius > 0.0f)
				{
					sel.outlineWorldOffset = std::max(0.01f, bounds.sphereRadius * 0.03f);
				}
			}

			if (di.material.id != 0)
			{
				const auto& mat = scene.GetMaterial(di.material);
				const MaterialPerm perm = EffectivePerm(mat);
				sel.isTransparent = HasFlag(perm, MaterialPerm::Transparent);
			}
			else
			{
				sel.isTransparent = false;
			}

			const std::uint32_t startInstance = static_cast<std::uint32_t>(selectionInstances.size());
			selectionInstances.push_back(sel.instance);

			if (sel.isTransparent)
			{
				selectionTransparent.push_back(sel);
				selectionTransparentStart.push_back(startInstance);
			}
			else
			{
				selectionOpaque.push_back(sel);
				selectionOpaqueStart.push_back(startInstance);
			}
		}

		// Upload all selection instances at once. Note: rhi::Device::UpdateBuffer is deferred on DX12,
		// so per-draw updates would not interleave correctly with draw calls.
		if (!selectionInstances.empty() && highlightInstanceBuffer_)
		{
			device_.UpdateBuffer(highlightInstanceBuffer_, std::as_bytes(std::span{ selectionInstances }));
		}

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
			bool usingReflectionProbeEnv = false;
			rhi::TextureHandle envArrayTexture{};
			if (batch.materialHandle.id != 0)
			{
				const auto& mat = scene.GetMaterial(batch.materialHandle);

				if (mat.envSource == EnvSource::ReflectionCapture)
				{
					if (settings_.enableReflectionCapture)
					{
						if (batch.reflectionProbeIndex >= 0 &&
							static_cast<std::size_t>(batch.reflectionProbeIndex) < reflectionProbes_.size())
						{
							const auto& probe = reflectionProbes_[static_cast<std::size_t>(batch.reflectionProbeIndex)];
							if (probe.cubeDescIndex != 0 && probe.cube)
							{
								envDescIndex = probe.cubeDescIndex;
								envArrayTexture = probe.cube;
								usingReflectionProbeEnv = true;
							}
						}
						else if (reflectionCubeDescIndex_ != 0 && reflectionCube_)
						{
							envDescIndex = reflectionCubeDescIndex_;
							envArrayTexture = reflectionCube_;
							usingReflectionProbeEnv = true;
						}
					}
				}
				else // EnvSource::Skybox
				{
					envDescIndex = scene.skyboxDescIndex;
				}
			}

			ctx.commandList.BindTextureDesc(17, envDescIndex);

			if (usingReflectionProbeEnv && envArrayTexture)
			{
				ctx.commandList.BindTexture2DArray(18, envArrayTexture);
			}

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
			}
			if (settings_.enableReflectionCapture && usingReflectionProbeEnv)
			{
				// Dynamic reflection capture cubemap: render mip0 only -> force mip0 sampling in shader.
				flags |= kFlagEnvForceMip0;
				// Dynamic probe is sampled through manual face+UV mapping (gEnvArray).
				// Keep an explicit runtime toggle bit for axis convention correction.
				flags |= kFlagEnvFlipZ;
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

			float envSourceForGBuffer = 0.0f; // 0 = Skybox, 1 = ReflectionCapture
			float probeIdxNForGBuffer = 0.0f; // reserved for future multi-probe deferred path
			if (settings_.enableReflectionCapture &&
				reflectionCubeDescIndex_ != 0 &&
				batch.materialHandle.id != 0)
			{
				const auto& mat = scene.GetMaterial(batch.materialHandle);
				if (mat.envSource == EnvSource::ReflectionCapture &&
					batch.reflectionProbeIndex >= 0)
				{
					envSourceForGBuffer = 1.0f;
					// DeferredLighting_dx12.hlsl currently does not use envSel.y yet,
					// but keep writing a stable normalized probe index into GBuffer3.y
					// so the buffer contents are no longer misleading and future multi-probe
					// work has the data available.
					probeIdxNForGBuffer =
						std::min(1.0f, static_cast<float>(batch.reflectionProbeIndex + 1) / 255.0f);
				}
			}

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

			constants.uEnvProbeBoxMin = { 0.0f, 0.0f, 0.0f, envSourceForGBuffer };
			constants.uEnvProbeBoxMax = { 0.0f, 0.0f, 0.0f, probeIdxNForGBuffer };

			if (usingReflectionProbeEnv && batch.reflectionProbeIndex >= 0 &&
				static_cast<std::size_t>(batch.reflectionProbeIndex) < reflectionProbes_.size())
			{
				const auto& probe = reflectionProbes_[static_cast<std::size_t>(batch.reflectionProbeIndex)];
				const float h = settings_.reflectionProbeBoxHalfExtent;

				constants.uEnvProbeBoxMin = {
					probe.capturePos.x - h,
					probe.capturePos.y - h,
					probe.capturePos.z - h,
					0.0f
				};

				constants.uEnvProbeBoxMax = {
					probe.capturePos.x + h,
					probe.capturePos.y + h,
					probe.capturePos.z + h,
					0.0f
				};
			}

			// IA (instanced)
			ctx.commandList.BindInputLayout(batch.mesh->layoutInstanced);
			ctx.commandList.BindVertexBuffer(0, batch.mesh->vertexBuffer, batch.mesh->vertexStrideBytes, 0);
			ctx.commandList.BindVertexBuffer(1, instanceBuffer_, instStride, batch.instanceOffset * instStride);
			ctx.commandList.BindIndexBuffer(batch.mesh->indexBuffer, batch.mesh->indexType, 0);

			ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &constants, 1 }));
			ctx.commandList.DrawIndexed(batch.mesh->indexCount, batch.mesh->indexType, 0, 0, batch.instanceCount, 0);
		}


		// --- Planar reflections (stencil-gated overlay; coplanar mirrors are grouped to avoid seams) ---
#include "RendererImpl/DirectX12Renderer_RenderFrame_04a_PlanarReflections.inl"

	// If selected objects are opaque, draw outline/highlight BEFORE transparent objects
	// so transparent surfaces still blend on top.
		if (!selectionOpaque.empty())
		{
			DrawSelectionGroup(selectionOpaque, selectionOpaqueStart);
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
				bool usingReflectionProbeEnv = false;
				rhi::TextureHandle envArrayTexture{};
				rhi::TextureDescIndex envDescIndex = scene.skyboxDescIndex;
				if (batchTransparent.materialHandle.id != 0)
				{
					const auto& mat = scene.GetMaterial(batchTransparent.materialHandle);

					if (mat.envSource == EnvSource::ReflectionCapture && settings_.enableReflectionCapture)
					{
						if (reflectionCubeDescIndex_ != 0 && reflectionCube_)
						{
							envDescIndex = reflectionCubeDescIndex_;
							envArrayTexture = reflectionCube_;
							usingReflectionProbeEnv = true;
						}
					}
				}
				ctx.commandList.BindTextureDesc(17, envDescIndex);

				if (usingReflectionProbeEnv && envArrayTexture)
				{
					ctx.commandList.BindTexture2DArray(18, envArrayTexture);
				}

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
					// Dynamic reflection captures update only mip0. Force mip0 sampling in the shader to
					// avoid seams/garbage when the cube texture was created with a full mip chain.
					if (settings_.enableReflectionCapture && usingReflectionProbeEnv)
					{
						flags |= kFlagEnvForceMip0;
						flags |= kFlagEnvFlipZ;
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
				constants.uEnvProbeBoxMin = { 0.0f, 0.0f, 0.0f, 0.0f };
				constants.uEnvProbeBoxMax = { 0.0f, 0.0f, 0.0f, 0.0f };

				ctx.commandList.BindInputLayout(batchTransparent.mesh->layoutInstanced);
				ctx.commandList.BindVertexBuffer(0, batchTransparent.mesh->vertexBuffer, batchTransparent.mesh->vertexStrideBytes, 0);
				ctx.commandList.BindVertexBuffer(1, instanceBuffer_, instStride, batchTransparent.instanceOffset * instStride);
				ctx.commandList.BindIndexBuffer(batchTransparent.mesh->indexBuffer, batchTransparent.mesh->indexType, 0);

				ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &constants, 1 }));

				// IMPORTANT: transparent = one object per draw (instanceCount = 1)
				ctx.commandList.DrawIndexed(batchTransparent.mesh->indexCount, batchTransparent.mesh->indexType, 0, 0, 1, 0);
			}
		}

		// If selected objects are transparent, render outline/highlight AFTER the transparent pass
		// so it stays visible on top of their own translucency.
		if (!selectionTransparent.empty())
		{
			DrawSelectionGroup(selectionTransparent, selectionTransparentStart);
		}

	});

	if (canForwardSSAO)
	{
		const auto sceneColorAO = graph.CreateTexture(renderGraph::RGTextureDesc{
			.extent = scDesc.extent,
			.format = rhi::Format::RGBA8_UNORM,
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
		const float aspect = scDesc.extent.height
			? (static_cast<float>(scDesc.extent.width) / static_cast<float>(scDesc.extent.height))
			: 1.0f;
		const mathUtils::Mat4 proj = mathUtils::PerspectiveRH_ZO(
			mathUtils::DegToRad(scene.camera.fovYDeg),
			aspect,
			scene.camera.nearZ,
			scene.camera.farZ);
		const mathUtils::Mat4 view = mathUtils::LookAt(
			scene.camera.position,
			scene.camera.target,
			scene.camera.up);
		const mathUtils::Mat4 viewProj = proj * view;
		const mathUtils::Mat4 invViewProj = mathUtils::Inverse(viewProj);
		const mathUtils::Mat4 invViewProjT = mathUtils::Transpose(invViewProj);

		struct alignas(16) FogConstants
		{
			std::array<float, 16> uInvViewProj{};
			std::array<float, 4> uCameraPos{};
			std::array<float, 4> uFogParams{};
			std::array<float, 4> uFogColor{};
		};
		static_assert(sizeof(FogConstants) % 16 == 0);

		FogConstants c{};
		std::memcpy(c.uInvViewProj.data(), mathUtils::ValuePtr(invViewProjT), sizeof(float) * 16);
		c.uCameraPos = { scene.camera.position.x, scene.camera.position.y, scene.camera.position.z, 0.0f };
		c.uFogParams = { settings_.fogStart, settings_.fogEnd, settings_.fogDensity, static_cast<float>(settings_.fogMode) };
		c.uFogColor = { settings_.fogColor[0], settings_.fogColor[1], settings_.fogColor[2], settings_.enableFog ? 1.0f : 0.0f };

		const auto sceneColorFog = graph.CreateTexture(renderGraph::RGTextureDesc{
			.extent = scDesc.extent,
			.format = rhi::Format::RGBA8_UNORM,
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

	{
		rhi::ClearDesc clear{};
		clear.clearColor = false;
		clear.clearDepth = false;
		clear.clearStencil = false;

		graph.AddSwapChainPass("ForwardPresent", clear, [this, forwardSceneColorAfterPost](renderGraph::PassContext& ctx)
			{
				const auto extent = ctx.passExtent;
				ctx.commandList.SetViewport(0, 0, static_cast<int>(extent.width), static_cast<int>(extent.height));
				ctx.commandList.SetState(copyToSwapChainState_);
				ctx.commandList.BindInputLayout(fullscreenLayout_);
				ctx.commandList.BindPipeline(psoCopyToSwapChain_);
				ctx.commandList.BindTexture2D(0, ctx.resources.GetTexture(forwardSceneColorAfterPost));
				ctx.commandList.Draw(3, 0);
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