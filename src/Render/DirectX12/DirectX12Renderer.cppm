module;

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

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

			rhi::ClearDesc clearDesc{};
			clearDesc.clearColor = true;
			clearDesc.clearDepth = true;
			clearDesc.color = { 0.1f, 0.1f, 0.1f, 1.0f };

			graph.AddSwapChainPass("MainPass", clearDesc,
				[this, sampledTexture, sampledTextureDescIndex](renderGraph::PassContext& ctx)
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

					struct alignas(16) PerDrawConstants
					{
						std::array<float, 16> uMVP{};
						std::array<float, 4>  uColor{ 1.0f, 1.0f, 1.0f, 1.0f };
						int uUseTex{ 0 };
						int _pad[3]{};
					};
					PerDrawConstants constants{};

					const float timeS = TimeSeconds();
					const float aspect = extent.height
						? (static_cast<float>(extent.width) / static_cast<float>(extent.height))
						: 1.0f;

					glm::mat4 proj = glm::perspective(glm::radians(60.0f), aspect, 0.01f, 200.0f);
					glm::mat4 view = glm::lookAt(glm::vec3(2.2f, 1.6f, 2.2f), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
					glm::mat4 model = glm::rotate(glm::mat4(1.0f), 0.8f, glm::vec3(0, 1, 0));
					glm::mat4 mvp = proj * view * model;

					std::memcpy(constants.uMVP.data(), glm::value_ptr(mvp), sizeof(float) * 16);

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

					// DX12 doesn't use the name-based uniform path. The renderer packs
					// a small per-draw constant block and sends raw bytes to the backend.
					ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &constants, 1 }));

					if (hasIndices)
						ctx.commandList.DrawIndexed(mesh_.indexCount, mesh_.indexType, 0, 0);
					else
						ctx.commandList.Draw(static_cast<std::uint32_t>(cpuFallbackVertexCount_), 0);
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

		static float TimeSeconds()
		{
			using clock = std::chrono::steady_clock;
			static const auto start = clock::now();
			const auto now = clock::now();
			return std::chrono::duration<float>(now - start).count();
		}

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

			// Pick shaders per backend
			std::filesystem::path vertexShaderPath;
			std::filesystem::path pixelShaderPath;

			switch (device_.GetBackend())
			{
			case rhi::Backend::DirectX12:
				// One HLSL file for both stages (VSMain/PSMain inside)
				vertexShaderPath = corefs::ResolveAsset("shaders\\Mesh_dx12.hlsl");
				pixelShaderPath = vertexShaderPath;
				break;

			case rhi::Backend::OpenGL:
			default:
				vertexShaderPath = corefs::ResolveAsset("shaders\\Mesh.vert");
				pixelShaderPath = corefs::ResolveAsset("shaders\\Mesh.frag");
				break;
			}

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

		//------------------------------------------------------------------------------------------------------------------//
		rhi::IRHIDevice& device_;
		RendererSettings settings_{};

		ShaderLibrary shaderLibrary_;
		PSOCache psoCache_;

		MeshRHI mesh_{};
		rhi::PipelineHandle pso_{};
		rhi::GraphicsState state_{};

		std::size_t cpuFallbackVertexCount_{ 0 };
	};
}
