// Main pipeline permutations (UseTex / UseShadow)
{
	auto MakeDefines = [](bool useTex, bool useShadow) -> std::vector<std::string>
		{
			std::vector<std::string> defines;
			if (useTex)
			{
				defines.push_back("USE_TEX=1");
			}
			if (useShadow)
			{
				defines.push_back("USE_SHADOW=1");
			}
			return defines;
		};

	for (std::uint32_t idx = 0; idx < 4; ++idx)
	{
		const bool useTex = (idx & 1u) != 0;
		const bool useShadow = (idx & 2u) != 0;
		const auto defs = MakeDefines(useTex, useShadow);

		const auto vs = shaderLibrary_.GetOrCreateShader(ShaderKey{
			.stage = rhi::ShaderStage::Vertex,
			.name = "VSMain",
			.filePath = shaderPath.string(),
			.defines = defs
			});
		const auto ps = shaderLibrary_.GetOrCreateShader(ShaderKey{
			.stage = rhi::ShaderStage::Pixel,
			.name = "PSMain",
			.filePath = shaderPath.string(),
			.defines = defs
			});

		std::string psoName = "PSO_Mesh";
		if (useTex)
		{
			psoName += "_Tex";
		}
		if (useShadow)
		{
			psoName += "_Shadow";
		}

		psoMain_[idx] = psoCache_.GetOrCreate(psoName, vs, ps);

		// Planar reflection variant: same shader but with CORE_PLANAR_CLIP enabled (VS outputs SV_ClipDistance0).
		{
			auto planarDefs = defs;
			planarDefs.push_back("CORE_PLANAR_CLIP=1");
			const auto vsPlanar = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Vertex,
				.name = "VSMain",
				.filePath = shaderPath.string(),
				.defines = planarDefs
				});
			const auto psPlanar = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Pixel,
				.name = "PSMain",
				.filePath = shaderPath.string(),
				.defines = planarDefs
				});
			psoPlanar_[idx] = psoCache_.GetOrCreate(psoName + "_Planar", vsPlanar, psPlanar);
		}
	}

	// Editor selection highlight overlay (unlit).
	{
		const std::vector<std::string> hiDefs = { "CORE_HIGHLIGHT=1" };
		const auto vsHi = shaderLibrary_.GetOrCreateShader(ShaderKey{
			.stage = rhi::ShaderStage::Vertex,
			.name = "VSMain",
			.filePath = shaderPath.string(),
			.defines = hiDefs
			});
		const auto psHi = shaderLibrary_.GetOrCreateShader(ShaderKey{
			.stage = rhi::ShaderStage::Pixel,
			.name = "PSMain",
			.filePath = shaderPath.string(),
			.defines = hiDefs
			});
		psoHighlight_ = psoCache_.GetOrCreate("PSO_Mesh_Highlight", vsHi, psHi);

		auto outlineDefs = hiDefs;
		outlineDefs.push_back("CORE_OUTLINE=1");
		const auto vsOutline = shaderLibrary_.GetOrCreateShader(ShaderKey{
			.stage = rhi::ShaderStage::Vertex,
			.name = "VSMain",
			.filePath = shaderPath.string(),
			.defines = outlineDefs
			});
		const auto psOutline = shaderLibrary_.GetOrCreateShader(ShaderKey{
			.stage = rhi::ShaderStage::Pixel,
			.name = "PSMain",
			.filePath = shaderPath.string(),
			.defines = outlineDefs
			});
		psoOutline_ = psoCache_.GetOrCreate("PSO_Mesh_Outline", vsOutline, psOutline);
	}

	state_.depth.testEnable = true;
	state_.depth.writeEnable = true;
	state_.depth.depthCompareOp = rhi::CompareOp::LessEqual;
	state_.rasterizer.cullMode = rhi::CullMode::Back;
	state_.rasterizer.frontFace = rhi::FrontFace::CounterClockwise;
	state_.blend.enable = false;
	transparentState_ = state_;
	transparentState_.depth.writeEnable = false;
	transparentState_.blend.enable = true;
	transparentState_.rasterizer.cullMode = rhi::CullMode::None;


	particleState_ = transparentState_;
	particleState_.blend.mode = rhi::BlendMode::Additive;
	particleState_.rasterizer.cullMode = rhi::CullMode::None;
	// Depth pre-pass state: same raster as opaque, depth test+write enabled.
	preDepthState_ = state_;

	// Main pass state when running after a depth pre-pass: keep depth read-only.
	mainAfterPreDepthState_ = state_;
	mainAfterPreDepthState_.depth.writeEnable = false;

	// Highlight pass: translucent overlay, depth-tested but depth read-only.
	highlightState_ = state_;
	highlightState_.blend.enable = true;
	highlightState_.depth.writeEnable = false;
	highlightState_.rasterizer.cullMode = rhi::CullMode::None;

	// Outline mark pass: write stencil where the selected object is visible, keep color untouched
	// by drawing with alpha=0 under standard alpha blending.
	outlineMarkState_ = state_;
	outlineMarkState_.blend.enable = true;
	outlineMarkState_.depth.writeEnable = false;
	outlineMarkState_.rasterizer.cullMode = rhi::CullMode::None;
	outlineMarkState_.depth.stencil.enable = true;
	outlineMarkState_.depth.stencil.readMask = 0xFFu;
	outlineMarkState_.depth.stencil.writeMask = 0xFFu;
	outlineMarkState_.depth.stencil.front.failOp = rhi::StencilOp::Keep;
	outlineMarkState_.depth.stencil.front.depthFailOp = rhi::StencilOp::Keep;
	outlineMarkState_.depth.stencil.front.passOp = rhi::StencilOp::Replace;
	outlineMarkState_.depth.stencil.front.compareOp = rhi::CompareOp::Always;
	outlineMarkState_.depth.stencil.back = outlineMarkState_.depth.stencil.front;

	// Outline shell pass: render an inflated version of the selected mesh only where the
	// stencil is NOT equal to the selected-object mark. Front-face culling keeps the shell
	// mostly on the silhouette.
	outlineState_ = state_;
	outlineState_.blend.enable = true;
	outlineState_.depth.writeEnable = false;
	outlineState_.rasterizer.cullMode = rhi::CullMode::Front;
	outlineState_.depth.stencil.enable = true;
	outlineState_.depth.stencil.readMask = 0xFFu;
	outlineState_.depth.stencil.writeMask = 0x00u;
	outlineState_.depth.stencil.front.failOp = rhi::StencilOp::Keep;
	outlineState_.depth.stencil.front.depthFailOp = rhi::StencilOp::Keep;
	outlineState_.depth.stencil.front.passOp = rhi::StencilOp::Keep;
	outlineState_.depth.stencil.front.compareOp = rhi::CompareOp::NotEqual;
	outlineState_.depth.stencil.back = outlineState_.depth.stencil.front;

	// Planar reflection stencil mask (writes stencil, keeps color untouched via depth-only PSO).
	planarMaskState_ = preDepthState_;
	planarMaskState_.rasterizer.cullMode = rhi::CullMode::Front;
	planarMaskState_.depth.testEnable = true;
	planarMaskState_.depth.writeEnable = false;
	planarMaskState_.depth.depthCompareOp = rhi::CompareOp::LessEqual;
	planarMaskState_.blend.enable = false;
	planarMaskState_.rasterizer.cullMode = rhi::CullMode::None;
	planarMaskState_.depth.stencil.enable = true;
	planarMaskState_.depth.stencil.readMask = 0x01u;
	planarMaskState_.depth.stencil.writeMask = 0x01u;
	planarMaskState_.depth.stencil.front.failOp = rhi::StencilOp::Keep;
	planarMaskState_.depth.stencil.front.depthFailOp = rhi::StencilOp::Keep;
	planarMaskState_.depth.stencil.front.passOp = rhi::StencilOp::Replace;
	planarMaskState_.depth.stencil.front.compareOp = rhi::CompareOp::Always;
	planarMaskState_.depth.stencil.back = planarMaskState_.depth.stencil.front;

	// Reflected scene pass: stencil-gated overlay inside visible mirror pixels (MVP path).
	planarReflectedState_ = state_;
	// Reflected scene overlay: stencil-gated, depth-tested against the main depth buffer.
	planarReflectedState_.depth.testEnable = true;
	planarReflectedState_.depth.writeEnable = true;
	planarReflectedState_.depth.depthCompareOp = rhi::CompareOp::LessEqual;
	// Robust option: disable culling for the reflected pass (avoids winding issues when the view is mirrored).
	planarReflectedState_.rasterizer.cullMode = rhi::CullMode::Back;
	planarReflectedState_.rasterizer.frontFace = rhi::FrontFace::Clockwise;
	planarReflectedState_.blend.enable = false;
	planarReflectedState_.depth.stencil.enable = true;
	planarReflectedState_.depth.stencil.readMask = 0x01u;
	planarReflectedState_.depth.stencil.writeMask = 0x00u;
	planarReflectedState_.depth.stencil.front.failOp = rhi::StencilOp::Keep;
	planarReflectedState_.depth.stencil.front.depthFailOp = rhi::StencilOp::Keep;
	planarReflectedState_.depth.stencil.front.passOp = rhi::StencilOp::Keep;
	planarReflectedState_.depth.stencil.front.compareOp = rhi::CompareOp::Equal;
	planarReflectedState_.depth.stencil.back = planarReflectedState_.depth.stencil.front;
			}

// Debug cubemap atlas pipeline (fullscreen triangle with a tiny VB).
{
	const auto dbgPath = corefs::ResolveAsset("shaders\\DebugCubeAtlas_dx12.hlsl");

	const auto vsDbg = shaderLibrary_.GetOrCreateShader(ShaderKey{
		.stage = rhi::ShaderStage::Vertex,
		.name = "VSMain",
		.filePath = dbgPath.string(),
		.defines = {}
		});
	const auto psDbg = shaderLibrary_.GetOrCreateShader(ShaderKey{
		.stage = rhi::ShaderStage::Pixel,
		.name = "PSMain",
		.filePath = dbgPath.string(),
		.defines = {}
		});

	psoDebugCubeAtlas_ = psoCache_.GetOrCreate("PSO_DebugCubeAtlas", vsDbg, psDbg);

	debugCubeAtlasState_ = {};
	debugCubeAtlasState_.depth.testEnable = false;
	debugCubeAtlasState_.depth.writeEnable = false;
	debugCubeAtlasState_.blend.enable = false;
	debugCubeAtlasState_.rasterizer.cullMode = rhi::CullMode::None;
	debugCubeAtlasState_.rasterizer.frontFace = rhi::FrontFace::CounterClockwise;

	// Input layout: POSITION.xy + TEXCOORD0.xy
	rhi::InputLayoutDesc il{};
	il.strideBytes = 16;
	il.attributes = {
		rhi::VertexAttributeDesc{.semantic = rhi::VertexSemantic::Position, .semanticIndex = 0, .format = rhi::VertexFormat::R32G32_FLOAT, .inputSlot = 0, .offsetBytes = 0 },
		rhi::VertexAttributeDesc{.semantic = rhi::VertexSemantic::TexCoord, .semanticIndex = 0, .format = rhi::VertexFormat::R32G32_FLOAT, .inputSlot = 0, .offsetBytes = 8 },
	};
	debugCubeAtlasLayout_ = device_.CreateInputLayout(il);

	struct DebugFSVertex { float px, py; float ux, uy; };
	const DebugFSVertex tri[3] = {
		{ -1.0f, -1.0f, 0.0f, 0.0f },
		{ -1.0f,  3.0f, 0.0f, 2.0f },
		{  3.0f, -1.0f, 2.0f, 0.0f },
	};

	rhi::BufferDesc vbDesc{};
	vbDesc.bindFlag = rhi::BufferBindFlag::VertexBuffer;
	vbDesc.usageFlag = rhi::BufferUsageFlag::Default;
	vbDesc.sizeInBytes = sizeof(tri);
	vbDesc.debugName = "DebugCubeAtlasVB";
	debugCubeAtlasVB_ = device_.CreateBuffer(vbDesc);
	if (debugCubeAtlasVB_)
	{
		device_.UpdateBuffer(debugCubeAtlasVB_, std::as_bytes(std::span{ tri, 3 }));
		debugCubeAtlasVBStrideBytes_ = 16;
	}
	// Deferred rendering (DX12): GBuffer writer + fullscreen resolve.
	{
		const auto gbufPath = corefs::ResolveAsset("shaders\\DeferredGBuffer_dx12.hlsl");
		const auto lightPath = corefs::ResolveAsset("shaders\\DeferredLighting_dx12.hlsl");
		const auto ssaoPath = corefs::ResolveAsset("shaders\\SSAO_dx12.hlsl");
		const auto ssaoBlurPath = corefs::ResolveAsset("shaders\\SSAOBlur_dx12.hlsl");
		const auto ssaoCompositePath = corefs::ResolveAsset("shaders\\SSAOComposite_dx12.hlsl");
		const auto fogPath = corefs::ResolveAsset("shaders\\FogPost_dx12.hlsl");
		const auto bloomExtractPath = corefs::ResolveAsset("shaders\\BloomExtract_dx12.hlsl");
		const auto bloomBlurPath = corefs::ResolveAsset("shaders\\BloomBlur_dx12.hlsl");
		const auto bloomCompositePath = corefs::ResolveAsset("shaders\\BloomComposite_dx12.hlsl");
		const auto toneMapPath = corefs::ResolveAsset("shaders\\ToneMap_dx12.hlsl");
		const auto copyPath = corefs::ResolveAsset("shaders\\CopyToSwapChain_dx12.hlsl");
		const auto planarCompPath = corefs::ResolveAsset("shaders\\PlanarComposite_dx12.hlsl");
		const auto particlePath = corefs::ResolveAsset("shaders\\Particles_dx12.hlsl");

		const auto vsG = shaderLibrary_.GetOrCreateShader(ShaderKey{
			.stage = rhi::ShaderStage::Vertex,
			.name = "VS_GBuffer",
			.filePath = gbufPath.string(),
			.defines = {},
			.shaderModel = rhi::ShaderModel::SM6_1
			});
		const auto psG = shaderLibrary_.GetOrCreateShader(ShaderKey{
			.stage = rhi::ShaderStage::Pixel,
			.name = "PS_GBuffer",
			.filePath = gbufPath.string(),
			.defines = {},
			.shaderModel = rhi::ShaderModel::SM6_1
			});
		psoDeferredGBuffer_ = psoCache_.GetOrCreate("PSO_Deferred_GBuffer", vsG, psG);

		const auto vsFS = shaderLibrary_.GetOrCreateShader(ShaderKey{
			.stage = rhi::ShaderStage::Vertex,
			.name = "VS_Fullscreen",
			.filePath = lightPath.string(),
			.defines = {},
			.shaderModel = rhi::ShaderModel::SM6_1
			});
		const auto psFS = shaderLibrary_.GetOrCreateShader(ShaderKey{
			.stage = rhi::ShaderStage::Pixel,
			.name = "PS_DeferredLighting",
			.filePath = lightPath.string(),
			.defines = {},
			.shaderModel = rhi::ShaderModel::SM6_1
			});
		psoDeferredLighting_ = psoCache_.GetOrCreate("PSO_Deferred_Lighting", vsFS, psFS);

		// SSAO (R32_FLOAT) + depth-aware blur (R32_FLOAT), fullscreen passes.
		{
			const auto vsSSAO = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Vertex,
				.name = "VS_Fullscreen",
				.filePath = ssaoPath.string(),
				.defines = {},
				.shaderModel = rhi::ShaderModel::SM6_1
				});
			const auto psSSAO = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Pixel,
				.name = "PS_SSAO",
				.filePath = ssaoPath.string(),
				.defines = {},
				.shaderModel = rhi::ShaderModel::SM6_1
				});
			psoSSAO_ = psoCache_.GetOrCreate("PSO_SSAO", vsSSAO, psSSAO);
		}
		{
			const std::vector<std::string> defs = { "FORWARD_SSAO_FROM_DEPTH=0" };

			const auto vsSSAOForward = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Vertex,
				.name = "VS_Fullscreen",
				.filePath = ssaoPath.string(),
				.defines = defs,
				.shaderModel = rhi::ShaderModel::SM6_1
				});

			const auto psSSAOForward = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Pixel,
				.name = "PS_SSAO",
				.filePath = ssaoPath.string(),
				.defines = defs,
				.shaderModel = rhi::ShaderModel::SM6_1
				});

			psoSSAOForward_ = psoCache_.GetOrCreate("PSO_SSAO_Forward", vsSSAOForward, psSSAOForward);
		}
		{
			const auto vsB = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Vertex,
				.name = "VS_Fullscreen",
				.filePath = ssaoBlurPath.string(),
				.defines = {},
				.shaderModel = rhi::ShaderModel::SM6_1
				});
			const auto psB = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Pixel,
				.name = "PS_SSAOBlur",
				.filePath = ssaoBlurPath.string(),
				.defines = {},
				.shaderModel = rhi::ShaderModel::SM6_1
				});
			psoSSAOBlur_ = psoCache_.GetOrCreate("PSO_SSAO_Blur", vsB, psB);
		}
		{
			const auto vsC = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Vertex,
				.name = "VS_Fullscreen",
				.filePath = ssaoCompositePath.string(),
				.defines = {},
				.shaderModel = rhi::ShaderModel::SM6_1
				});

			const auto psC = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Pixel,
				.name = "PS_SSAOComposite",
				.filePath = ssaoCompositePath.string(),
				.defines = {},
				.shaderModel = rhi::ShaderModel::SM6_1
				});

			psoSSAOComposite_ = psoCache_.GetOrCreate("PSO_SSAO_Composite", vsC, psC);
		}
		// Fog (post effect): fullscreen pass using SceneColor + depth.
		{
			const auto vsFog = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Vertex,
				.name = "VS_Fullscreen",
				.filePath = fogPath.string(),
				.defines = {},
				.shaderModel = rhi::ShaderModel::SM6_1
				});
			const auto psFog = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Pixel,
				.name = "PS_Fog",
				.filePath = fogPath.string(),
				.defines = {},
				.shaderModel = rhi::ShaderModel::SM6_1
				});
			psoFog_ = psoCache_.GetOrCreate("PSO_Fog", vsFog, psFog);
		}

		// Bloom (extract -> blur -> composite) in HDR scene color.
		{
			const auto vsBloomExtract = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Vertex,
				.name = "VS_Fullscreen",
				.filePath = bloomExtractPath.string(),
				.defines = {},
				.shaderModel = rhi::ShaderModel::SM6_1
				});
			const auto psBloomExtract = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Pixel,
				.name = "PS_BloomExtract",
				.filePath = bloomExtractPath.string(),
				.defines = {},
				.shaderModel = rhi::ShaderModel::SM6_1
				});
			psoBloomExtract_ = psoCache_.GetOrCreate("PSO_Bloom_Extract", vsBloomExtract, psBloomExtract);
		}
		{
			const auto vsBloomBlur = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Vertex,
				.name = "VS_Fullscreen",
				.filePath = bloomBlurPath.string(),
				.defines = {},
				.shaderModel = rhi::ShaderModel::SM6_1
				});
			const auto psBloomBlur = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Pixel,
				.name = "PS_BloomBlur",
				.filePath = bloomBlurPath.string(),
				.defines = {},
				.shaderModel = rhi::ShaderModel::SM6_1
				});
			psoBloomBlur_ = psoCache_.GetOrCreate("PSO_Bloom_Blur", vsBloomBlur, psBloomBlur);
		}
		{
			const auto vsBloomComposite = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Vertex,
				.name = "VS_Fullscreen",
				.filePath = bloomCompositePath.string(),
				.defines = {},
				.shaderModel = rhi::ShaderModel::SM6_1
				});
			const auto psBloomComposite = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Pixel,
				.name = "PS_BloomComposite",
				.filePath = bloomCompositePath.string(),
				.defines = {},
				.shaderModel = rhi::ShaderModel::SM6_1
				});
			psoBloomComposite_ = psoCache_.GetOrCreate("PSO_Bloom_Composite", vsBloomComposite, psBloomComposite);
		}
		{
			const auto vsToneMap = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Vertex,
				.name = "VS_Fullscreen",
				.filePath = toneMapPath.string(),
				.defines = {},
				.shaderModel = rhi::ShaderModel::SM6_1
				});
			const auto psToneMap = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Pixel,
				.name = "PS_ToneMap",
				.filePath = toneMapPath.string(),
				.defines = {},
				.shaderModel = rhi::ShaderModel::SM6_1
				});
			psoToneMap_ = psoCache_.GetOrCreate("PSO_ToneMap", vsToneMap, psToneMap);
		}
		// Billboard particles: procedural + textured variant.
		{
			const auto vsParticles = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Vertex,
				.name = "VSMain",
				.filePath = particlePath.string(),
				.defines = {},
				.shaderModel = rhi::ShaderModel::SM6_1
				});
			const auto psParticles = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Pixel,
				.name = "PSMain",
				.filePath = particlePath.string(),
				.defines = {},
				.shaderModel = rhi::ShaderModel::SM6_1
				});
			psoParticles_ = psoCache_.GetOrCreate("PSO_Particles", vsParticles, psParticles);

			const std::vector<std::string> texturedDefs = { "PARTICLE_TEXTURED=1" };
			const auto vsParticlesTextured = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Vertex,
				.name = "VSMain",
				.filePath = particlePath.string(),
				.defines = texturedDefs,
				.shaderModel = rhi::ShaderModel::SM6_1
				});
			const auto psParticlesTextured = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Pixel,
				.name = "PSMain",
				.filePath = particlePath.string(),
				.defines = texturedDefs,
				.shaderModel = rhi::ShaderModel::SM6_1
				});
			psoParticlesTextured_ = psoCache_.GetOrCreate("PSO_Particles_Textured", vsParticlesTextured, psParticlesTextured);
		}

		// Copy scene color to swapchain (fullscreen blit).
		{
			const auto copyPath = corefs::ResolveAsset("shaders\\CopyToSwapChain_dx12.hlsl");
			const auto vsCopy = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Vertex,
				.name = "VS_Fullscreen",
				.filePath = copyPath.string(),
				.defines = {},
				.shaderModel = rhi::ShaderModel::SM6_1
				});
			const auto psCopy = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Pixel,
				.name = "PS_CopyToSwapChain",
				.filePath = copyPath.string(),
				.defines = {},
				.shaderModel = rhi::ShaderModel::SM6_1
				});
			psoCopyToSwapChain_ = psoCache_.GetOrCreate("PSO_CopyToSwapChain", vsCopy, psCopy);
		}

		// Empty input layout for fullscreen triangle (SV_VertexID only).
		rhi::InputLayoutDesc il{};
		il.strideBytes = 0;
		il.attributes = {};
		il.debugName = "Fullscreen_NoInput";
		fullscreenLayout_ = device_.CreateInputLayout(il);

		deferredLightingState_ = {};
		deferredLightingState_.depth.testEnable = false;
		deferredLightingState_.depth.writeEnable = false;
		deferredLightingState_.blend.enable = false;
		deferredLightingState_.rasterizer.cullMode = rhi::CullMode::None;
		deferredLightingState_.rasterizer.frontFace = rhi::FrontFace::CounterClockwise;

		// Copy SceneColor -> swapchain (fullscreen)
		{
			const auto vsCopy = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Vertex,
				.name = "VS_Fullscreen",
				.filePath = copyPath.string(),
				.defines = {}
				});
			const auto psCopy = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Pixel,
				.name = "PS_CopyToSwapChain",
				.filePath = copyPath.string(),
				.defines = {}
				});
			psoCopyToSwapChain_ = psoCache_.GetOrCreate("PSO_CopyToSwapChain", vsCopy, psCopy);
			copyToSwapChainState_ = deferredLightingState_;
		}

		// Planar composite (mask+color -> SceneColor), fullscreen alpha blend.
		{
			const auto vsPC = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Vertex,
				.name = "VS_Fullscreen",
				.filePath = planarCompPath.string(),
				.defines = {}
				});
			const auto psPC = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Pixel,
				.name = "PS_PlanarComposite",
				.filePath = planarCompPath.string(),
				.defines = {}
				});
			psoPlanarComposite_ = psoCache_.GetOrCreate("PSO_PlanarComposite", vsPC, psPC);
			planarCompositeState_ = deferredLightingState_;
			planarCompositeState_.blend.enable = true;

			// Stencil-gate the composite to only the mirror pixels written by planarMaskState_.
			planarCompositeState_.depth.stencil.enable = true;
			planarCompositeState_.depth.stencil.readMask = 0x01u;
			planarCompositeState_.depth.stencil.writeMask = 0x00u;
			planarCompositeState_.depth.stencil.front.failOp = rhi::StencilOp::Keep;
			planarCompositeState_.depth.stencil.front.depthFailOp = rhi::StencilOp::Keep;
			planarCompositeState_.depth.stencil.front.passOp = rhi::StencilOp::Keep;
			planarCompositeState_.depth.stencil.front.compareOp = rhi::CompareOp::Equal;
			planarCompositeState_.depth.stencil.back = planarCompositeState_.depth.stencil.front;
		}
	}
	}