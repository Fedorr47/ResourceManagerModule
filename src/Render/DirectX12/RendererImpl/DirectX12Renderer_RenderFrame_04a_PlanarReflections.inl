if (settings_.enablePlanarReflections && !planarMirrorDraws.empty())
{
	const mathUtils::Mat4 viewProjT = mathUtils::Transpose(viewProj);

	constexpr std::uint32_t kMaterialFlagUseTex = 1u << 0;
	constexpr std::uint32_t kMaterialFlagUseShadow = 1u << 1;
	constexpr std::uint32_t kMaterialFlagUseNormal = 1u << 2;
	constexpr std::uint32_t kMaterialFlagUseMetalTex = 1u << 3;
	constexpr std::uint32_t kMaterialFlagUseRoughTex = 1u << 4;
	constexpr std::uint32_t kMaterialFlagUseAOTex = 1u << 5;
	constexpr std::uint32_t kMaterialFlagUseEmissiveTex = 1u << 6;
	constexpr std::uint32_t kMaterialFlagUseEnv = 1u << 7;
	constexpr std::uint32_t kMaterialFlagEnvForceMip0 = 1u << 8;
	constexpr std::uint32_t kMaterialFlagEnvFlipZ = 1u << 9;

	std::uint32_t mirrorIndex = 0u;

	for (const PlanarMirrorDraw& mirror : planarMirrorDraws)
	{
		if (!mirror.mesh || mirror.mesh->indexCount == 0)
		{
			continue;
		}

		if (mirrorIndex >= settings_.planarReflectionMaxMirrors)
		{
			break;
		}

		auto [planeN, planeD] = CanonicalizePlane(mirror.planeNormal, mirror.planePoint);
		if (mathUtils::Dot(planeN, camPosLocal) + planeD < 0.0f)
		{
			planeN = -planeN;
			planeD = -planeD;
		}
		
		// ---------------- (1) Stencil mask: visible mirror pixels -> stencil = ref ----------------
		ctx.commandList.SetState(planarMaskState_);
		ctx.commandList.SetStencilRef(1u + mirrorIndex);
		ctx.commandList.BindPipeline(psoShadow_);

		SingleMatrixPassConstants maskConstants{};
		std::memcpy(maskConstants.uLightViewProj.data(), mathUtils::ValuePtr(viewProjT), sizeof(float) * 16);
		ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &maskConstants, 1 }));

		ctx.commandList.BindInputLayout(mirror.mesh->layoutInstanced);
		ctx.commandList.BindVertexBuffer(0, mirror.mesh->vertexBuffer, mirror.mesh->vertexStrideBytes, 0);
		ctx.commandList.BindVertexBuffer(1, instanceBuffer_, instStride, mirror.instanceOffset* instStride);
		ctx.commandList.BindIndexBuffer(mirror.mesh->indexBuffer, mirror.mesh->indexType, 0);
		ctx.commandList.DrawIndexed(mirror.mesh->indexCount, mirror.mesh->indexType, 0, 0, 1, 0);

		// ---------------- (2) Reflected scene: reflected camera, stencil-gated ----------------
		const mathUtils::Mat4 reflectW = mathUtils::MakeReflectionMatrix(planeN, planeD);
		const mathUtils::Mat4 viewProjRefl = viewProj * reflectW;
		const mathUtils::Mat4 viewProjReflT = mathUtils::Transpose(viewProjRefl);
		
		ctx.commandList.SetState(planarReflectedState_);
		ctx.commandList.SetStencilRef(1u + mirrorIndex);

		// ---------------------------------------------------------------------
		// Skybox in planar reflection (FORWARD path).
		//
		// Important:
		// - We are reusing the main depth buffer (mirror plane already wrote depth),
		//   so the regular skybox pass won't fill the mirror region.
		// - Draw skybox here, stencil-gated to the mirror, with depth-test disabled.
		// ---------------------------------------------------------------------
		if (scene.skyboxDescIndex != 0)
		{
			const auto extent = ctx.passExtent;
			const float aspect = extent.height
				? (static_cast<float>(extent.width) / static_cast<float>(extent.height))
				: 1.0f;

			const mathUtils::Mat4 proj = mathUtils::PerspectiveRH_ZO(
				mathUtils::DegToRad(scene.camera.fovYDeg),
				aspect,
				scene.camera.nearZ,
				scene.camera.farZ);

			mathUtils::Mat4 viewNoTranslation =
				mathUtils::LookAt(scene.camera.position, scene.camera.target, scene.camera.up);
			viewNoTranslation[3] = mathUtils::Vec4(0.0f, 0.0f, 0.0f, 1.0f);

			mathUtils::Mat4 reflectDir = reflectW;
			reflectDir[3] = mathUtils::Vec4(0.0f, 0.0f, 0.0f, 1.0f);

			const mathUtils::Mat4 viewProjSkyReflT =
				mathUtils::Transpose((proj * viewNoTranslation) * reflectDir);

			SkyboxConstants skyConsts{};
			std::memcpy(
				skyConsts.uViewProj.data(),
				mathUtils::ValuePtr(viewProjSkyReflT),
				sizeof(float) * 16);

			rhi::GraphicsState skyState = skyboxState_;

			// Stencil gate to current mirror pixels.
			skyState.depth.stencil.enable = true;
			skyState.depth.stencil.readMask = 0xFFu;
			skyState.depth.stencil.writeMask = 0x00u;
			skyState.depth.stencil.front.failOp = rhi::StencilOp::Keep;
			skyState.depth.stencil.front.depthFailOp = rhi::StencilOp::Keep;
			skyState.depth.stencil.front.passOp = rhi::StencilOp::Keep;
			skyState.depth.stencil.front.compareOp = rhi::CompareOp::Equal;
			skyState.depth.stencil.back = skyState.depth.stencil.front;

			// Disable depth test/write for planar skybox fill.
			skyState.depth.testEnable = false;
			skyState.depth.writeEnable = false;

			ctx.commandList.SetState(skyState);
			ctx.commandList.SetStencilRef(1u + mirrorIndex);

			ctx.commandList.BindPipeline(psoSkybox_);
			ctx.commandList.BindTextureDesc(0, scene.skyboxDescIndex);
			ctx.commandList.BindInputLayout(skyboxMesh_.layout);
			ctx.commandList.BindVertexBuffer(0, skyboxMesh_.vertexBuffer, skyboxMesh_.vertexStrideBytes, 0);
			ctx.commandList.BindIndexBuffer(skyboxMesh_.indexBuffer, skyboxMesh_.indexType, 0);
			ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &skyConsts, 1 }));
			ctx.commandList.DrawIndexed(skyboxMesh_.indexCount, skyboxMesh_.indexType, 0, 0);

			// Restore reflected mesh state.
			ctx.commandList.SetState(planarReflectedState_);
			ctx.commandList.SetStencilRef(1u + mirrorIndex);
		}

		const auto& planarBatches = !captureMainBatchesNoCull.empty() ? captureMainBatchesNoCull : mainBatches;

		for (const Batch& batch : planarBatches)
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
				if (batch.material.albedoDescIndex != 0)
				{
					perm = perm | MaterialPerm::UseTex;
				}
			}
			if (HasFlag(perm, MaterialPerm::PlanarMirror))
			{
				continue; // avoid self-recursion in planar path
			}

			const bool useTex = HasFlag(perm, MaterialPerm::UseTex);
			const bool useShadow = HasFlag(perm, MaterialPerm::UseShadow);

			// Use the regular main pipeline (no special planar defines).
			ctx.commandList.BindPipeline(PlanarPipelineFor(perm));
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
						if (batch.reflectionProbeIndex >= 0 && static_cast<std::size_t>(batch.reflectionProbeIndex) < reflectionProbes_.size())
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
				else
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
			if (useTex) flags |= kMaterialFlagUseTex;
			if (useShadow) flags |= kMaterialFlagUseShadow;
			if (batch.material.normalDescIndex != 0) flags |= kMaterialFlagUseNormal;
			if (batch.material.metalnessDescIndex != 0) flags |= kMaterialFlagUseMetalTex;
			if (batch.material.roughnessDescIndex != 0) flags |= kMaterialFlagUseRoughTex;
			if (batch.material.aoDescIndex != 0) flags |= kMaterialFlagUseAOTex;
			if (batch.material.emissiveDescIndex != 0) flags |= kMaterialFlagUseEmissiveTex;
			if (envDescIndex != 0) flags |= kMaterialFlagUseEnv;
			if (settings_.enableReflectionCapture && usingReflectionProbeEnv)
			{
				flags |= kMaterialFlagEnvForceMip0;
				flags |= kMaterialFlagEnvFlipZ;
			}

			PerBatchConstants constants{};
			// plane: n·x + d = 0, and we keep (n·x + d) >= 0
			const mathUtils::Vec3 clipN = planeN;
			const float clipD = planeD - 0.05f;

			const mathUtils::Mat4 dirVP_T = mathUtils::Transpose(dirLightViewProj);
			std::memcpy(constants.uViewProj.data(), mathUtils::ValuePtr(viewProjReflT), sizeof(float) * 16);
			std::memcpy(constants.uLightViewProj.data(), mathUtils::ValuePtr(dirVP_T), sizeof(float) * 16);
			const mathUtils::Vec3 camPosRefl = ReflectPoint(camPosLocal, planeN, planeD);
			constants.uCameraAmbient = { camPosRefl.x, camPosRefl.y, camPosRefl.z, 0.22f };
			const mathUtils::Vec3 camFwdRefl = ReflectVector(camFLocal, planeN);
			constants.uCameraForward = { camFwdRefl.x, camFwdRefl.y, camFwdRefl.z, clipN.z };
			constants.uBaseColor = { batch.material.baseColor.x, batch.material.baseColor.y, batch.material.baseColor.z, batch.material.baseColor.w };
			
			const float materialBiasTexels = batch.material.shadowBias;
			constants.uMaterialFlags = { clipN.x, clipN.y, materialBiasTexels, AsFloatBits(flags) };

			constants.uPbrParams = { batch.material.metallic, batch.material.roughness, batch.material.ao, batch.material.emissiveStrength };
			constants.uCounts = { float(lightCount), float(spotShadows.size()), float(pointShadows.size()), clipD };
			constants.uShadowBias = { settings_.dirShadowBaseBiasTexels, settings_.spotShadowBaseBiasTexels, settings_.pointShadowBaseBiasTexels, settings_.shadowSlopeScaleTexels };
			constants.uEnvProbeBoxMin = { 0.0f, 0.0f, 0.0f, 0.0f };
			constants.uEnvProbeBoxMax = { 0.0f, 0.0f, 0.0f, 0.0f };
			if (usingReflectionProbeEnv && batch.reflectionProbeIndex >= 0 &&
				static_cast<std::size_t>(batch.reflectionProbeIndex) < reflectionProbes_.size())
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

		++mirrorIndex;
	}

	// Restore state for the following passes (transparent / imgui).
	ctx.commandList.SetState(doDepthPrepass ? mainAfterPreDepthState_ : state_);
	ctx.commandList.SetStencilRef(0);
}