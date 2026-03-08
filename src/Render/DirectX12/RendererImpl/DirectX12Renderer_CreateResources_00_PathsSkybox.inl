			std::filesystem::path shaderPath;
			std::filesystem::path shadowPath;
			std::filesystem::path pointShadowPath;

			switch (device_.GetBackend())
			{
			case rhi::Backend::DirectX12:
				shaderPath = corefs::ResolveAsset("shaders\\GlobalShaderInstanced_dx12.hlsl");
				shadowPath = corefs::ResolveAsset("shaders\\ShadowDepth_dx12.hlsl");
				pointShadowPath = corefs::ResolveAsset("shaders\\ShadowPoint_dx12.hlsl");
				break;
			default:
				shaderPath = corefs::ResolveAsset("shaders\\VS.vert");
				shadowPath = corefs::ResolveAsset("shaders\\VS.vert");
				break;
			}

			std::filesystem::path skyboxPath = corefs::ResolveAsset("shaders\\Skybox_dx12.hlsl");

			// Skybox shaders
			const auto vsSky = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Vertex,
				.name = "VS_Skybox",
				.filePath = skyboxPath.string(),
				.defines = {}
				});
			const auto psSky = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Pixel,
				.name = "PS_Skybox",
				.filePath = skyboxPath.string(),
				.defines = {}
				});

			psoSkybox_ = psoCache_.GetOrCreate("PSO_Skybox", vsSky, psSky);

			skyboxState_.depth.testEnable = true;
			skyboxState_.depth.writeEnable = false;
			skyboxState_.depth.depthCompareOp = rhi::CompareOp::LessEqual;

			skyboxState_.rasterizer.cullMode = rhi::CullMode::None;
			skyboxState_.rasterizer.frontFace = rhi::FrontFace::CounterClockwise;

			skyboxState_.blend.enable = false;

			// skybox mesh
			{
				MeshCPU skyCpu = MakeSkyboxCubeCPU();
				skyboxMesh_ = UploadMesh(device_, skyCpu, "SkyboxCube_DX12");
			}

			// Unit quad for instanced billboard particles.
			{
				MeshCPU particleCpu{};
				particleCpu.vertices = {
					VertexDesc{ -0.5f, -0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f },
					VertexDesc{  0.5f, -0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f },
					VertexDesc{  0.5f,  0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f },
					VertexDesc{ -0.5f,  0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f },
				};
				particleCpu.indices = { 0u, 1u, 2u, 2u, 3u, 0u };
				particleMesh_ = UploadMesh(device_, particleCpu, "ParticleQuad_DX12");
			}

