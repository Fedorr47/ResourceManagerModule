module;

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
// D3D-style clip-space helpers (Z in [0..1]).
// NOTE: glm::perspective()/ortho() default to OpenGL clip space (Z in [-1..1]),
// which in D3D12 will clip most geometry and may make objects appear tiny or disappear.
#include <glm/ext/matrix_clip_space.hpp>

#include <array>
#include <span>
#include <chrono>
#include <string>
#include <filesystem>
#include <cstring>
#include <utility>

export module core:renderer_dx12;

import :rhi;
import :renderer_settings;
import :render_core;
import :render_graph;
import :file_system;
import :mesh;
import :obj_loader;

export namespace rendern
{
	class DX12Renderer
	{
	public:
		DX12Renderer(rhi::IRHIDevice& device, RendererSettings settings = {})
			: device_(device)
			, settings_(std::move(settings))
			, shaderLibrary_(device)
			, psoCache_(device)
		{
			CreateResources();
		}

		// Render a frame
		// If 'sampledTextureDescIndex' != 0 -> it is used (bindless-style path).
		// Else if 'sampledTexture' is not null -> it is bound at slot 0 and sampled.
		void RenderFrame(
			rhi::IRHISwapChain& swapChain,
			rhi::TextureHandle sampledTexture = {},
			rhi::TextureDescIndex sampledTextureDescIndex = 0)
		{
			renderGraph::RenderGraph graph;

			// ---------------- Shadow map (depth-only pass) ----------------
			const rhi::Extent2D shadowExtent{ 2048, 2048 };

			const auto shadowRG = graph.CreateTexture(renderGraph::RGTextureDesc{
				.extent = shadowExtent,
				.format = rhi::Format::D32_FLOAT,
				.usage = renderGraph::ResourceUsage::DepthStencil,
				.debugName = "ShadowMap"
				});

			// Shared light-space setup (directional light shadow)
			const glm::vec3 lightDir = glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f)); // FROM light towards the scene
			const glm::vec3 center = glm::vec3(0.0f, 0.0f, 0.0f);
			const float lightDist = 10.0f;
			const glm::vec3 lightPos = center - lightDir * lightDist;
			const glm::mat4 lightView = glm::lookAt(lightPos, center, glm::vec3(0, 1, 0));

			const float orthoHalf = 8.0f;
			// D3D clip space expects Z in [0..1].
			const glm::mat4 lightProj = glm::orthoRH_ZO(-orthoHalf, orthoHalf, -orthoHalf, orthoHalf, 0.1f, 40.0f);

			// Scene transforms (cube + ground plane)
			const glm::mat4 cubeModel =
				glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.6f, 0.0f)) *
				glm::rotate(glm::mat4(1.0f), 0.8f, glm::vec3(0, 1, 0));

			const glm::mat4 groundModel =
				glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -0.6f, 0.0f)) *
				glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(1, 0, 0)) *
				glm::scale(glm::mat4(1.0f), glm::vec3(8.0f, 8.0f, 8.0f));

			{
				rhi::ClearDesc clear{};
				clear.clearColor = false;
				clear.clearDepth = true;
				clear.depth = 1.0f;

				renderGraph::PassAttachments att{};
				att.useSwapChainBackbuffer = false;
				att.color = std::nullopt;
				att.depth = shadowRG;
				att.clearDesc = clear;

				graph.AddPass("ShadowPass", std::move(att),
					[this, lightView, lightProj, cubeModel, groundModel](renderGraph::PassContext& ctx)
					{
						ctx.commandList.SetViewport(0, 0,
							static_cast<int>(ctx.passExtent.width),
							static_cast<int>(ctx.passExtent.height));

						ctx.commandList.SetState(shadowState_);
						ctx.commandList.BindPipeline(psoShadow_);

						struct alignas(16) ShadowConstants
						{
							std::array<float, 16> uMVP{}; // for Shadow_dx12.hlsl: uMVP == lightMVP
						};

						auto DrawShadowMesh = [&](const MeshRHI& mesh, const glm::mat4& model)
							{
								ctx.commandList.BindInputLayout(mesh.layout);
								ctx.commandList.BindVertexBuffer(0, mesh.vertexBuffer, mesh.vertexStrideBytes, 0);

								const bool hasIndices = (mesh.indexBuffer.id != 0) && (mesh.indexCount != 0);
								if (hasIndices)
								{
									ctx.commandList.BindIndexBuffer(mesh.indexBuffer, mesh.indexType, 0);
								}

								const glm::mat4 lightMVP = lightProj * lightView * model;

								ShadowConstants constants{};
								std::memcpy(constants.uMVP.data(), glm::value_ptr(lightMVP), sizeof(float) * 16);
								ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &constants, 1 }));

								if (hasIndices)
								{
									ctx.commandList.DrawIndexed(mesh.indexCount, mesh.indexType, 0, 0);
								}
								else
								{
									ctx.commandList.Draw(static_cast<std::uint32_t>(cpuFallbackVertexCount_), 0);
								}
							};

						// Draw ground first (so it's in the map) then the cube
						DrawShadowMesh(groundMesh_, groundModel);
						DrawShadowMesh(cubeMesh_, cubeModel);
					});
			}

			// ---------------- Main pass (swapchain) ----------------
			rhi::ClearDesc clearDesc{};
			clearDesc.clearColor = true;
			clearDesc.clearDepth = true;
			clearDesc.color = { 0.1f, 0.1f, 0.1f, 1.0f };

			graph.AddSwapChainPass("MainPass", clearDesc,
				[this, sampledTexture, sampledTextureDescIndex, shadowRG, lightView, lightProj, lightDir, cubeModel, groundModel](renderGraph::PassContext& ctx)
				{
					const auto extent = ctx.passExtent;

					ctx.commandList.SetViewport(0, 0,
						static_cast<int>(extent.width),
						static_cast<int>(extent.height));

					ctx.commandList.SetState(state_);
					ctx.commandList.BindPipeline(pso_);

					// Shadow map (slot1 / t1)
					{
						const auto shadowTex = ctx.resources.GetTexture(shadowRG);
						ctx.commandList.BindTexture2D(1, shadowTex);
					}

					// Optional albedo texture (slot0 / t0)
					const bool hasAlbedo =
						(sampledTextureDescIndex != 0) || (sampledTexture.id != 0);

					if (sampledTextureDescIndex != 0)
					{
						ctx.commandList.BindTextureDesc(0, sampledTextureDescIndex);
					}
					else if (sampledTexture.id != 0)
					{
						ctx.commandList.BindTexture2D(0, sampledTexture);
					}

					// ---------------- Per-draw constants (256 bytes) ----------------
					// Must match assets/shaders/GlobalShader_dx12.hlsl (cbuffer PerDraw : b0)
					struct alignas(16) PerDrawShadowedDirConstants
					{
						std::array<float, 16> uMVP{};         // 64
						std::array<float, 16> uLightMVP{};    // 64
						std::array<float, 12> uModelRows{};   // 48

						std::array<float, 4>  uCameraAmbient{};    // cam.xyz, ambientStrength
						std::array<float, 4>  uBaseColor{};        // rgba
						std::array<float, 4>  uMaterialFlags{};    // shininess, specStrength, shadowBias, flagsPacked
						std::array<float, 4>  uDir_DirIntensity{}; // dir.xyz (FROM light), intensity
						std::array<float, 4>  uDir_Color{};        // rgb, unused
					};
					static_assert(sizeof(PerDrawShadowedDirConstants) == 256);

					auto WriteRow = [](const glm::mat4& m, int row, std::array<float, 12>& outRows, int rowIndex)
						{
							// GLM is column-major: m[col][row]
							outRows[rowIndex * 4 + 0] = m[0][row];
							outRows[rowIndex * 4 + 1] = m[1][row];
							outRows[rowIndex * 4 + 2] = m[2][row];
							outRows[rowIndex * 4 + 3] = m[3][row];
						};

					constexpr std::uint32_t kFlagUseTex = 1u << 0;
					constexpr std::uint32_t kFlagUseShadow = 1u << 1;
					constexpr std::uint32_t kFlagDirLight = 1u << 2;

					const float aspect = extent.height
						? (static_cast<float>(extent.width) / static_cast<float>(extent.height))
						: 1.0f;

					// Camera
					const glm::vec3 camPos = glm::vec3(2.2f, 1.6f, 2.2f);
					const glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(60.0f), aspect, 0.01f, 200.0f);
					const glm::mat4 view = glm::lookAt(camPos, glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));

					auto DrawLitMesh = [&](const MeshRHI& mesh, const glm::mat4& model, const glm::vec4& baseColor, bool useTex)
						{
							ctx.commandList.BindInputLayout(mesh.layout);
							ctx.commandList.BindVertexBuffer(0, mesh.vertexBuffer, mesh.vertexStrideBytes, 0);

							const bool hasIndices = (mesh.indexBuffer.id != 0) && (mesh.indexCount != 0);
							if (hasIndices)
							{
								ctx.commandList.BindIndexBuffer(mesh.indexBuffer, mesh.indexType, 0);
							}

							PerDrawShadowedDirConstants constants{};

							const glm::mat4 mvp = proj * view * model;
							const glm::mat4 lightMVP = lightProj * lightView * model;

							std::memcpy(constants.uMVP.data(), glm::value_ptr(mvp), sizeof(float) * 16);
							std::memcpy(constants.uLightMVP.data(), glm::value_ptr(lightMVP), sizeof(float) * 16);

							WriteRow(model, 0, constants.uModelRows, 0);
							WriteRow(model, 1, constants.uModelRows, 1);
							WriteRow(model, 2, constants.uModelRows, 2);

							// Camera + ambient
							constants.uCameraAmbient = { camPos.x, camPos.y, camPos.z, 0.22f };

							// Base color
							constants.uBaseColor = { baseColor.x, baseColor.y, baseColor.z, baseColor.w };

							// Flags
							std::uint32_t flags = 0;
							flags |= kFlagDirLight;
							flags |= kFlagUseShadow;
							if (useTex && hasAlbedo)
							{
								flags |= kFlagUseTex;
							}

							// Material
							const float shininess = useTex ? 64.0f : 32.0f;
							const float specStrength = useTex ? 0.5f : 0.15f;
							const float shadowBias = 0.0015f;

							constants.uMaterialFlags = {
								shininess,
								specStrength,
								shadowBias,
								static_cast<float>(flags)
							};

							// Directional light
							constants.uDir_DirIntensity = { lightDir.x, lightDir.y, lightDir.z, 1.2f };
							constants.uDir_Color = { 1.0f, 1.0f, 1.0f, 0.0f };

							ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &constants, 1 }));

							if (hasIndices)
							{
								ctx.commandList.DrawIndexed(mesh.indexCount, mesh.indexType, 0, 0);
							}
							else
							{
								ctx.commandList.Draw(static_cast<std::uint32_t>(cpuFallbackVertexCount_), 0);
							}
						};

					// Ground first, then cube
					DrawLitMesh(groundMesh_, groundModel, glm::vec4(0.8f, 0.8f, 0.8f, 1.0f), false);
					DrawLitMesh(cubeMesh_, cubeModel, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), true);
				});

			graph.Execute(device_, swapChain);
			swapChain.Present();
		}


		void Shutdown()
		{
			DestroyMesh(device_, cubeMesh_);
			DestroyMesh(device_, groundMesh_);
			psoCache_.ClearCache();
			shaderLibrary_.ClearCache();
		}

	private:
		void CreateResources()
		{
			// -------- Cube (main mesh) --------
			MeshCPU cubeCpu{};
			try
			{
				const auto modelAbs = corefs::ResolveAsset(settings_.modelPath);
				cubeCpu = LoadObj(modelAbs);
			}
			catch (...)
			{
				cubeCpu.vertices = {
					VertexDesc{-0.8f,-0.6f,0, 0,0,1, 0,0},
					VertexDesc{ 0.8f,-0.6f,0, 0,0,1, 1,0},
					VertexDesc{ 0.0f, 0.9f,0, 0,0,1, 0.5f,1},
				};
				cubeCpu.indices = { 0,1,2 };
			}
			cpuFallbackVertexCount_ = cubeCpu.vertices.size();
			cubeMesh_ = UploadMesh(device_, cubeCpu, "CubeMesh");

			// -------- Ground (shadow receiver) --------
			MeshCPU groundCpu{};
			try
			{
				const auto quadAbs = corefs::ResolveAsset(std::filesystem::path("models") / "quad.obj");
				groundCpu = LoadObj(quadAbs);
			}
			catch (...)
			{
				// Fallback quad in XY plane (will be rotated to XZ)
				groundCpu.vertices = {
					VertexDesc{-1,-1,0, 0,0,1, 0,0},
					VertexDesc{ 1,-1,0, 0,0,1, 1,0},
					VertexDesc{ 1, 1,0, 0,0,1, 1,1},
					VertexDesc{-1, 1,0, 0,0,1, 0,1},
				};
				groundCpu.indices = { 0,1,2, 0,2,3 };
			}
			groundMesh_ = UploadMesh(device_, groundCpu, "GroundMesh");

			std::filesystem::path vertexShaderPath;
			std::filesystem::path pixelShaderPath;
			std::filesystem::path shadowShaderPath;

			switch (device_.GetBackend())
			{
			case rhi::Backend::DirectX12:
				vertexShaderPath = corefs::ResolveAsset("shaders\\GlobalShader_dx12.hlsl");
				pixelShaderPath = vertexShaderPath;
				shadowShaderPath = corefs::ResolveAsset("shaders\\Shadow_dx12.hlsl");
				break;

			case rhi::Backend::OpenGL:
			default:
				vertexShaderPath = corefs::ResolveAsset("shaders\\VS.vert");
				pixelShaderPath = corefs::ResolveAsset("shaders\\FS.frag");
				break;
			}

			// Main pipeline
			{
				const auto vertexShader = shaderLibrary_.GetOrCreateShader(ShaderKey{
					.stage = rhi::ShaderStage::Vertex,
					.name = "VS_Mesh",
					.filePath = vertexShaderPath.string(),
					.defines = {}
					});

				const auto pixelShader = shaderLibrary_.GetOrCreateShader(ShaderKey{
					.stage = rhi::ShaderStage::Pixel,
					.name = "PS_Mesh",
					.filePath = pixelShaderPath.string(),
					.defines = {} });

				pso_ = psoCache_.GetOrCreate("PSO_Mesh", vertexShader, pixelShader);

				state_.depth.testEnable = true;
				state_.depth.writeEnable = true;
				state_.depth.depthCompareOp = rhi::CompareOp::LessEqual;

				state_.rasterizer.cullMode = rhi::CullMode::None;
				state_.rasterizer.frontFace = rhi::FrontFace::CounterClockwise;

				state_.blend.enable = false;
			}

			// Shadow pipeline (DX12 only)
			if (device_.GetBackend() == rhi::Backend::DirectX12)
			{
				const auto vsShadow = shaderLibrary_.GetOrCreateShader(ShaderKey{
					.stage = rhi::ShaderStage::Vertex,
					.name = "VS_Shadow",
					.filePath = shadowShaderPath.string(),
					.defines = {} });

				const auto psShadow = shaderLibrary_.GetOrCreateShader(ShaderKey{
					.stage = rhi::ShaderStage::Pixel,
					.name = "PS_Shadow",
					.filePath = shadowShaderPath.string(),
					.defines = {} });

				psoShadow_ = psoCache_.GetOrCreate("PSO_Shadow", vsShadow, psShadow);

				shadowState_.depth.testEnable = true;
				shadowState_.depth.writeEnable = true;
				shadowState_.depth.depthCompareOp = rhi::CompareOp::LessEqual;

				// Disable culling for the demo so the ground plane always writes into the shadow map
				// (avoids winding/normal issues after rotating the quad).
				shadowState_.rasterizer.cullMode = rhi::CullMode::None;
				shadowState_.rasterizer.frontFace = rhi::FrontFace::CounterClockwise;

				shadowState_.blend.enable = false;
			}
		}

		//------------------------------------------------------------------------------------------------------------------//
		rhi::IRHIDevice& device_;
		RendererSettings settings_{};

		ShaderLibrary shaderLibrary_;
		PSOCache psoCache_;

		MeshRHI cubeMesh_{};
		MeshRHI groundMesh_{};

		// Main pass
		rhi::PipelineHandle pso_{};
		rhi::GraphicsState state_{};

		// Shadow pass
		rhi::PipelineHandle psoShadow_{};
		rhi::GraphicsState shadowState_{};

		std::size_t cpuFallbackVertexCount_{ 0 };
	};
} // namespace rendern
