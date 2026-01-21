module;

#include <array>
#include <span>
#include <string>

export module core:render_renderer;

import :rhi;
import :render_core;
import :render_graph;
import :scene_bridge;
import :file_system;

export namespace rendern
{
	struct RendererSettings
	{
		bool enableDepthPrepass{ false };
	};

	class Renderer
	{
	public:
		Renderer(rhi::IRHIDevice& device, RendererSettings settings = {})
			: device_(device)
			, settings_(settings)
			, shaderLibrary_(device)
			, psoCache_(device)
		{
			CreateFullScreenResources();
		}

		// Render a frame
		// if 'sampledTexture' is not null. it is bound at slot 0 and sampled
		// if 'sampledTexture' is null, it is preferred over the raw handle
		void RenderFrame(rhi::IRHISwapChain& swapChain, rhi::TextureHandle sampledTexture = {}, rhi::TextureDescIndex sampledTextureDescIndex = 0)
		{
			renderGraph::RenderGraph graph;

			rhi::ClearDesc clearDesc{};
			clearDesc.clearColor = true;
			clearDesc.clearDepth = false;
			clearDesc.color = { 0.1f, 0.1f, 0.1f, 1.0f };

			graph.AddSwapChainPass("MainPass", clearDesc, [this, sampledTexture, sampledTextureDescIndex](renderGraph::PassContext& ctx)
				{
					const auto extent = ctx.passExtent;
					ctx.commandList.SetViewport(0, 0, static_cast<int>(extent.width), static_cast<int>(extent.height));
					ctx.commandList.SetState(state_);
					ctx.commandList.BindPipeline(pso_);
					ctx.commandList.BindVertexArray(vao_);

					// Common uniforms
					ctx.commandList.SetUniformInt("uTex", 0);
					ctx.commandList.SetUniformFloat4("uColor", { 0.2f, 0.3f, 0.7f, 1.0f });

					if (sampledTextureDescIndex != 0)
					{
						ctx.commandList.BindTextureDesc(0, sampledTextureDescIndex);
						ctx.commandList.SetUniformInt("uUseTex", 1);
					}
					else if (sampledTexture.id != 0)
					{
						ctx.commandList.BindTexture2D(0, sampledTexture);
						ctx.commandList.SetUniformInt("uUseTex", 1);
					}
					else
					{
						ctx.commandList.SetUniformInt("uUseTex", 0);
					}

					ctx.commandList.Draw(3, 0);
				});

			graph.Execute(device_, swapChain);
			swapChain.Present();
		}

		void Shutdown()
		{
			psoCache_.ClearCache();

			if (vao_.id != 0)
			{
				device_.DestroyVertexArray(vao_);
				vao_ = {};
			}

			if (vbo_.id != 0)
			{
				device_.DestroyBuffer(vbo_);
				vbo_ = {};
			}
		}	

	private:

		void CreateFullScreenResources()
		{
			struct V
			{
				float x, y;
				float u, v;
			};

			const std::array<V, 3> verts
			{
				V{-1.0f, -1.0f, 0.0f, 0.0f},
				V{ 3.0f, -1.0f, 2.0f, 0.0f},
				V{-1.0f,  3.0f, 0.0f, 2.0f}
			};

			rhi::BufferDesc bufferDesc;
			bufferDesc.bindFlag = rhi::BufferBindFlag::VertexBuffer;
			bufferDesc.usageFlag = rhi::BufferUsageFlag::Static;
			bufferDesc.sizeInBytes = sizeof(verts);
			bufferDesc.debugName = "FullScreenVBO";

			vbo_ = device_.CreateBuffer(bufferDesc);
			device_.UpdateBuffer(vbo_, std::as_bytes(std::span{ verts }));

			vao_ = device_.CreateVertexArray("FullScreenVAO");

			// MVP: VertexAttribDesc.glType uses OpenGL GLenum values.
			// 0x1406 == GL_FLOAT.
			constexpr std::uint32_t GL_FLOAT_TYPE = 0x1406u;

			const std::array<rhi::VertexAttributeDesc, 2> attributes
			{
				rhi::VertexAttributeDesc{.location = 0, .componentCount = 2, .glType = GL_FLOAT_TYPE, .normalized = false, .strideBytes = sizeof(V), .offsetBytes = 0},
				rhi::VertexAttributeDesc{.location = 1, .componentCount = 2, .glType = GL_FLOAT_TYPE, .normalized = false, .strideBytes = sizeof(V), .offsetBytes = sizeof(float) * 2},
			};

			device_.SetVertexArrayLayout(vao_, vbo_, attributes);

			std::filesystem::path vertexShaderPath= corefs::ResolveAsset("shaders\\FullScreen.vert");
			std::filesystem::path pixelShaderPath= corefs::ResolveAsset("shaders\\FullScreen.frag");

			const auto vertexShader = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Vertex,
				.name = "VS_FullScreen",
				.filePath = vertexShaderPath.string(),
				.defines = {}
				});
			const auto pixelShader = shaderLibrary_.GetOrCreateShader(ShaderKey{ 
				.stage = rhi::ShaderStage::Pixel,
				.name = "PS_FullScreen",
				.filePath = pixelShaderPath.string(),
				.defines = {} });
			pso_ = psoCache_.GetOrCreate("PSO_FullScreen", vertexShader, pixelShader);

			state_.depth.testEnable = false;
			state_.depth.writeEnable = false;
			state_.rasterizer.cullMode = rhi::CullMode::None;
			state_.blend.enable = false;
		}

//------------------------------------------------------------------------------------------------------------------//
		rhi::IRHIDevice& device_;
		RendererSettings settings_{};

		ShaderLibrary shaderLibrary_;
		PSOCache psoCache_;

		rhi::PipelineHandle pso_{};
		rhi::BufferHandle vbo_{};
		rhi::VertexArrayHandle vao_{};
		rhi::GraphicsState state_{};
	};
}