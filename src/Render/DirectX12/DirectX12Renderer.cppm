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
					[this](renderGraph::PassContext& ctx)
					{
						ctx.commandList.SetViewport(0, 0,
							static_cast<int>(ctx.passExtent.width),
							static_cast<int>(ctx.passExtent.height));

						ctx.commandList.SetState(shadowState_);
						ctx.commandList.BindPipeline(psoShadow_);

						ctx.commandList.BindInputLayout(mesh_.layout);
						ctx.commandList.BindVertexBuffer(0, mesh_.vertexBuffer, mesh_.vertexStrideBytes, 0);

						const bool hasIndices = (mesh_.indexBuffer.id != 0) && (mesh_.indexCount != 0);
						if (hasIndices)
						{
							ctx.commandList.BindIndexBuffer(mesh_.indexBuffer, mesh_.indexType, 0);
						}

						// Light-space matrix (simple demo)
						const glm::vec3 lightDir = glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f));
						const glm::vec3 center = glm::vec3(0.0f, 0.0f, 0.0f);
						const glm::vec3 lightPos = center - lightDir * 6.0f;
						const glm::mat4 lightView = glm::lookAt(lightPos, center, glm::vec3(0, 1, 0));

						const float orthoHalf = 4.0f;
						// D3D clip space expects Z in [0..1].
						const glm::mat4 lightProj = glm::orthoRH_ZO(-orthoHalf, orthoHalf, -orthoHalf, orthoHalf, 0.1f, 20.0f);

						const glm::mat4 model = glm::rotate(glm::mat4(1.0f), 0.8f, glm::vec3(0, 1, 0));
						const glm::mat4 lightMVP = lightProj * lightView * model;

						struct alignas(16) ShadowConstants
						{
							std::array<float, 16> uMVP{}; // for Shadow_dx12.hlsl: uMVP == lightMVP
						} constants{};

						std::memcpy(constants.uMVP.data(), glm::value_ptr(lightMVP), sizeof(float) * 16);
						ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &constants, 1 }));

						if (hasIndices)
							ctx.commandList.DrawIndexed(mesh_.indexCount, mesh_.indexType, 0, 0);
						else
							ctx.commandList.Draw(static_cast<std::uint32_t>(cpuFallbackVertexCount_), 0);
					});
			}

			// ---------------- Main pass (swapchain) ----------------
			rhi::ClearDesc clearDesc{};
			clearDesc.clearColor = true;
			clearDesc.clearDepth = true;
			clearDesc.color = { 0.1f, 0.1f, 0.1f, 1.0f };

			graph.AddSwapChainPass("MainPass", clearDesc,
				[this, sampledTexture, sampledTextureDescIndex, shadowRG](renderGraph::PassContext& ctx)
				{
					const auto extent = ctx.passExtent;

					ctx.commandList.SetViewport(0, 0,
						static_cast<int>(extent.width),
						static_cast<int>(extent.height));

					ctx.commandList.SetState(state_);
					ctx.commandList.BindPipeline(pso_);

					ctx.commandList.BindInputLayout(mesh_.layout);
					ctx.commandList.BindVertexBuffer(0, mesh_.vertexBuffer, mesh_.vertexStrideBytes, 0);

					const bool hasIndices = (mesh_.indexBuffer.id != 0) && (mesh_.indexCount != 0);
					if (hasIndices)
					{
						ctx.commandList.BindIndexBuffer(mesh_.indexBuffer, mesh_.indexType, 0);
					}

					// Per-draw constants for Mesh_dx12.hlsl
					struct alignas(16) PerDrawConstants
					{
						std::array<float, 16> uMVP{};
						std::array<float, 16> uLightMVP{};
						std::array<float, 4>  uColor{ 1.0f, 1.0f, 1.0f, 1.0f };

						int   uUseTex{ 0 };
						int   uUseShadow{ 1 };
						float uShadowBias{ 0.0015f };
						float _pad0{ 0.0f };
					};
					PerDrawConstants constants{};

					const float aspect = extent.height
						? (static_cast<float>(extent.width) / static_cast<float>(extent.height))
						: 1.0f;

					// Camera
					// D3D clip space expects Z in [0..1]. Using glm::perspective() (OpenGL [-1..1])
					// can clip most geometry and make the mesh effectively invisible.
					const glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(60.0f), aspect, 0.01f, 200.0f);
					const glm::mat4 view = glm::lookAt(glm::vec3(2.2f, 1.6f, 2.2f), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
					const glm::mat4 model = glm::rotate(glm::mat4(1.0f), 0.8f, glm::vec3(0, 1, 0));

					// Light (must match ShadowPass)
					const glm::vec3 lightDir = glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f));
					const glm::vec3 center = glm::vec3(0.0f, 0.0f, 0.0f);
					const glm::vec3 lightPos = center - lightDir * 6.0f;
					const glm::mat4 lightView = glm::lookAt(lightPos, center, glm::vec3(0, 1, 0));
					const float orthoHalf = 4.0f;
					const glm::mat4 lightProj = glm::orthoRH_ZO(-orthoHalf, orthoHalf, -orthoHalf, orthoHalf, 0.1f, 20.0f);

					const glm::mat4 mvp = proj * view * model;
					const glm::mat4 lightMVP = lightProj * lightView * model;

					std::memcpy(constants.uMVP.data(), glm::value_ptr(mvp), sizeof(float) * 16);
					std::memcpy(constants.uLightMVP.data(), glm::value_ptr(lightMVP), sizeof(float) * 16);

					// Slot 0 = albedo, slot 1 = shadow map
					if (sampledTextureDescIndex != 0)
					{
						ctx.commandList.BindTextureDesc(0, sampledTextureDescIndex);
						constants.uUseTex = 1;
					}
					else if (sampledTexture)
					{
						ctx.commandList.BindTexture2D(0, sampledTexture);
						constants.uUseTex = 1;
					}
					else
					{
						constants.uUseTex = 0;
					}

					// Shadow texture (created by render graph)
					{
						const auto shadowTex = ctx.resources.GetTexture(shadowRG);
						ctx.commandList.BindTexture2D(1, shadowTex);
					}

					ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &constants, 1 }));

					if (hasIndices)
					{
						ctx.commandList.DrawIndexed(mesh_.indexCount, mesh_.indexType, 0, 0);
					}
					else
					{
						ctx.commandList.Draw(static_cast<std::uint32_t>(cpuFallbackVertexCount_), 0);
					}
				});

			graph.Execute(device_, swapChain);
			swapChain.Present();
		}

		void Shutdown()
		{
			DestroyMesh(device_, mesh_);
			psoCache_.ClearCache();
			shaderLibrary_.ClearCache();
		}

	private:
		void CreateResources()
		{
			MeshCPU cpu{};
			try
			{
				const auto modelAbs = corefs::ResolveAsset(settings_.modelPath);
				cpu = LoadObj(modelAbs);
			}
			catch (...)
			{
				cpu.vertices = {
					VertexDesc{-0.8f,-0.6f,0, 0,0,1, 0,0},
					VertexDesc{ 0.8f,-0.6f,0, 0,0,1, 1,0},
					VertexDesc{ 0.0f, 0.9f,0, 0,0,1, 0.5f,1},
				};
				cpu.indices = { 0,1,2 };
			}

			cpuFallbackVertexCount_ = cpu.vertices.size();
			mesh_ = UploadMesh(device_, cpu, "MainMesh");

			std::filesystem::path vertexShaderPath;
			std::filesystem::path pixelShaderPath;
			std::filesystem::path shadowShaderPath;

			switch (device_.GetBackend())
			{
			case rhi::Backend::DirectX12:
				vertexShaderPath = corefs::ResolveAsset("shaders\\Mesh_dx12.hlsl");
				pixelShaderPath = vertexShaderPath;
				shadowShaderPath = corefs::ResolveAsset("shaders\\Shadow_dx12.hlsl");
				break;

			case rhi::Backend::OpenGL:
			default:
				vertexShaderPath = corefs::ResolveAsset("shaders\\Mesh.vert");
				pixelShaderPath = corefs::ResolveAsset("shaders\\Mesh.frag");
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

				shadowState_.rasterizer.cullMode = rhi::CullMode::Back;
				shadowState_.rasterizer.frontFace = rhi::FrontFace::CounterClockwise;

				shadowState_.blend.enable = false;
			}
		}

		//------------------------------------------------------------------------------------------------------------------//
		rhi::IRHIDevice& device_;
		RendererSettings settings_{};

		ShaderLibrary shaderLibrary_;
		PSOCache psoCache_;

		MeshRHI mesh_{};

		// Main pass
		rhi::PipelineHandle pso_{};
		rhi::GraphicsState state_{};

		// Shadow pass
		rhi::PipelineHandle psoShadow_{};
		rhi::GraphicsState shadowState_{};

		std::size_t cpuFallbackVertexCount_{ 0 };
	};
} // namespace rendern
