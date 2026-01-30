module;

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>

export module core:renderer_mesh_gl;

import :rhi;
import :scene;
import :renderer_settings;
import :render_core;
import :render_graph;
import :file_system;
import :mesh;
import :obj_loader;

export namespace rendern
{
	class GLMeshRenderer
	{
	public:
		GLMeshRenderer(rhi::IRHIDevice& device, RendererSettings settings = {})
			: device_(device)
			, settings_(std::move(settings))
			, shaderLibrary_(device)
			, psoCache_(device)
		{
			CreateFallbackResources();
		}

		void RenderFrame(rhi::IRHISwapChain& swapChain, const Scene& scene)
		{
			renderGraph::RenderGraph graph;

			rhi::ClearDesc clearDesc{};
			clearDesc.clearColor = true;
			clearDesc.clearDepth = true;
			clearDesc.color = { 0.1f, 0.1f, 0.1f, 1.0f };

			graph.AddSwapChainPass(
				"MainPass",
				clearDesc,
				[this, &scene](renderGraph::PassContext& ctx)
				{
					const auto extent = ctx.passExtent;

					ctx.commandList.SetViewport(0, 0,
						static_cast<int>(extent.width),
						static_cast<int>(extent.height));

					ctx.commandList.SetState(state_);

					const float aspect = extent.height
						? (static_cast<float>(extent.width) / static_cast<float>(extent.height))
						: 1.0f;

					// OpenGL clip space: Z in [-1..1]
					const glm::mat4 proj = glm::perspective(glm::radians(scene.camera.fovYDeg), aspect, scene.camera.nearZ, scene.camera.farZ);
					const glm::mat4 view = glm::lookAt(scene.camera.position, scene.camera.target, scene.camera.up);

					auto DrawOne = [&](const MeshRHI& mesh, const glm::mat4& model, const MaterialParams& mat)
					{
						ctx.commandList.BindInputLayout(mesh.layout);
						ctx.commandList.BindVertexBuffer(0, mesh.vertexBuffer, mesh.vertexStrideBytes, 0);

						const bool hasIndices = (mesh.indexBuffer.id != 0) && (mesh.indexCount != 0);
						if (hasIndices)
							ctx.commandList.BindIndexBuffer(mesh.indexBuffer, mesh.indexType, 0);

						const bool useTex = (mat.albedoDescIndex != 0);

						// Pipeline permutation (compile-time flags). We still keep uUseTex uniform
						// as a runtime fallback if the shader uses it.
						ctx.commandList.BindPipeline(useTex ? psoTex_ : psoNoTex_);

						// Uniforms (name-based path; typical for GL)
						ctx.commandList.SetUniformInt("uTex", 0);
						ctx.commandList.SetUniformFloat4("uColor", { mat.baseColor.r, mat.baseColor.g, mat.baseColor.b, mat.baseColor.a });

						const glm::mat4 mvp = proj * view * model;

						std::array<float, 16> mvpArr{};
						std::memcpy(mvpArr.data(), glm::value_ptr(mvp), sizeof(float) * 16);
						ctx.commandList.SetUniformMat4("uMVP", mvpArr);

						if (useTex)
						{
							ctx.commandList.BindTextureDesc(0, mat.albedoDescIndex);
							ctx.commandList.SetUniformInt("uUseTex", 1);
						}
						else
						{
							ctx.commandList.SetUniformInt("uUseTex", 0);
						}

						if (hasIndices)
							ctx.commandList.DrawIndexed(mesh.indexCount, mesh.indexType, 0, 0);
						else
							ctx.commandList.Draw(static_cast<std::uint32_t>(cpuFallbackVertexCount_), 0);
					};

					// If no scene items were provided, draw the fallback mesh.
					if (scene.drawItems.empty())
					{
						const glm::mat4 model = glm::rotate(glm::mat4(1.0f), TimeSeconds() * 0.8f, glm::vec3(0, 1, 0));
						MaterialParams mat{};
						mat.baseColor = { 0.2f, 0.3f, 0.7f, 1.0f };
						DrawOne(mesh_, model, mat);
						return;
					}

					for (const auto& item : scene.drawItems)
					{
						if (!item.mesh)
							continue;

						MaterialParams mat{};
						if (item.material.id != 0)
						{
							mat = scene.GetMaterial(item.material).params;
						}
						else
						{
							mat.baseColor = { 1.0f, 1.0f, 1.0f, 1.0f };
						}

						const glm::mat4 model = item.transform.ToMatrix();
						DrawOne(*item.mesh, model, mat);
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
		static float TimeSeconds()
		{
			using clock = std::chrono::steady_clock;
			static const auto start = clock::now();
			const auto now = clock::now();
			return std::chrono::duration<float>(now - start).count();
		}

		void CreateFallbackResources()
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
			mesh_ = UploadMesh(device_, cpu, "FallbackMesh_GL");

			// Use existing shader names in assets/shaders/
			std::filesystem::path vertexShaderPath = corefs::ResolveAsset("shaders\\VS.vert");
			std::filesystem::path pixelShaderPath = corefs::ResolveAsset("shaders\\FS.frag");

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
				.defines = {}
				});

			// Permutation: USE_TEX=1
			const auto vertexShaderTex = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Vertex,
				.name = "VS_Mesh",
				.filePath = vertexShaderPath.string(),
				.defines = { "USE_TEX=1" }
				});
			const auto pixelShaderTex = shaderLibrary_.GetOrCreateShader(ShaderKey{
				.stage = rhi::ShaderStage::Pixel,
				.name = "PS_Mesh",
				.filePath = pixelShaderPath.string(),
				.defines = { "USE_TEX=1" }
				});

			psoTex_ = psoCache_.GetOrCreate("PSO_Mesh_Tex", vertexShaderTex, pixelShaderTex);

			psoNoTex_ = psoCache_.GetOrCreate("PSO_Mesh_NoTex", vertexShader, pixelShader);

			state_.depth.testEnable = true;
			state_.depth.writeEnable = true;
			state_.depth.depthCompareOp = rhi::CompareOp::LessEqual;

			state_.rasterizer.cullMode = rhi::CullMode::None;
			state_.rasterizer.frontFace = rhi::FrontFace::CounterClockwise;

			state_.blend.enable = false;
		}

		rhi::IRHIDevice& device_;
		RendererSettings settings_{};

		ShaderLibrary shaderLibrary_;
		PSOCache psoCache_;

		MeshRHI mesh_{}; // fallback-only
		rhi::PipelineHandle psoNoTex_{};
		rhi::PipelineHandle psoTex_{};
		rhi::GraphicsState state_{};

		std::size_t cpuFallbackVertexCount_{ 0 };
	};
} // namespace rendern
