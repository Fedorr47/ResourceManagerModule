module;

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <array>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>

export module core:renderer_mesh_gl;

import :rhi;
import :scene;
import :math_utils;
import :renderer_settings;
import :render_core;
import :visibility;
import :render_graph;
import :file_system;
import :mesh;

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

		void SetSettings(const RendererSettings& settings)
		{
			settings_ = settings;
		}

		void RenderFrame(rhi::IRHISwapChain& swapChain, const Scene& scene)
		{
			renderGraph::RenderGraph graph;

			rhi::ClearDesc clearDesc{};
			clearDesc.clearColor = true;
			clearDesc.clearDepth = true;
			clearDesc.clearStencil = true;
			clearDesc.stencil = 0;
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

					auto ToGlmVec3 = [](const mathUtils::Vec3& v) -> glm::vec3
						{
							return glm::vec3(v.x, v.y, v.z);
						};

					auto ToGlmMat4 = [](const mathUtils::Mat4& m) -> glm::mat4
						{
							glm::mat4 r(1.0f);
							for (int c = 0; c < 4; ++c)
							{
								for (int row = 0; row < 4; ++row)
								{
									r[c][row] = m[c][row];
								}
							}
							return r;
						};

					// OpenGL clip space: Z in [-1..1]
					const glm::mat4 proj = glm::perspective(glm::radians(scene.camera.fovYDeg), aspect, scene.camera.nearZ, scene.camera.farZ);
					const glm::mat4 view = glm::lookAt(ToGlmVec3(scene.camera.position), ToGlmVec3(scene.camera.target), ToGlmVec3(scene.camera.up));

					// Culling frustum (world-space). We intentionally build it via our mathUtils
					// D3D-style projection; it represents the same geometric frustum.
					const mathUtils::Mat4 cullProj = mathUtils::PerspectiveRH_ZO(mathUtils::DegToRad(scene.camera.fovYDeg), aspect, scene.camera.nearZ, scene.camera.farZ);
					const mathUtils::Mat4 cullView = mathUtils::LookAt(scene.camera.position, scene.camera.target, scene.camera.up);
					const mathUtils::Frustum cameraFrustum = mathUtils::ExtractFrustumRH_ZO(cullProj * cullView);
					const bool doFrustumCulling = settings_.enableFrustumCulling;

					// --- Skybox draw ---
					{
						glm::mat4 viewNoTrans = view;
						viewNoTrans[3] = glm::vec4(0, 0, 0, 1);

						glm::mat4 vp = proj * viewNoTrans;

						std::array<float, 16> vpArr{};
						std::memcpy(vpArr.data(), glm::value_ptr(vp), sizeof(float) * 16);

						ctx.commandList.SetState(skyboxState_);
						ctx.commandList.BindPipeline(psoSkybox_);

						ctx.commandList.BindInputLayout(skyboxMesh_.layout);
						ctx.commandList.BindVertexBuffer(0, skyboxMesh_.vertexBuffer, skyboxMesh_.vertexStrideBytes, 0);
						ctx.commandList.BindIndexBuffer(skyboxMesh_.indexBuffer, skyboxMesh_.indexType, 0);

						ctx.commandList.SetUniformMat4("uVP", vpArr);

						if (scene.skyboxDescIndex != 0)
						{
							ctx.commandList.BindTextureDesc(0, scene.skyboxDescIndex); // slot t0
						}
						ctx.commandList.DrawIndexed(skyboxMesh_.indexCount, skyboxMesh_.indexType, 0, 0);

						ctx.commandList.SetState(state_);
					}

					constexpr std::uint32_t kEditorOutlineStencilRef = 0x80u;

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
						ctx.commandList.SetUniformFloat4("uColor", { mat.baseColor.x, mat.baseColor.y, mat.baseColor.z, mat.baseColor.w });

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

					bool drewAny = false;

					for (std::size_t drawItemIndex = 0; drawItemIndex < scene.drawItems.size(); ++drawItemIndex)
					{
						const auto& item = scene.drawItems[drawItemIndex];
						if (!item.mesh)
							continue;

						const MeshRHI& mesh = item.mesh->GetResource();
						// Pending / not uploaded yet.
						if (mesh.vertexBuffer.id == 0 || mesh.indexCount == 0)
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

						const mathUtils::Mat4 modelMu = item.transform.ToMatrix();
						if (!IsVisible(item.mesh.get(), modelMu, cameraFrustum, doFrustumCulling))
						{
							continue;
						}
						const glm::mat4 model = ToGlmMat4(modelMu);
						DrawOne(mesh, model, mat);

						// Editor selection outline + highlight.
						if (scene.editorSelectedDrawItem >= 0
							&& static_cast<std::size_t>(scene.editorSelectedDrawItem) == drawItemIndex)
						{
							MaterialParams mark = mat;
							mark.baseColor = { 1.0f, 1.0f, 1.0f, 0.0f };
							mark.albedoDescIndex = 0;

							ctx.commandList.SetStencilRef(kEditorOutlineStencilRef);
							ctx.commandList.SetState(outlineMarkState_);
							DrawOne(mesh, model, mark);

							const auto& bounds = item.mesh->GetBounds();
							float outlineScale = 1.03f;
							if (bounds.sphereRadius > 0.0f)
							{
								outlineScale = std::clamp(1.0f + 0.08f / bounds.sphereRadius, 1.02f, 1.08f);
							}
							const glm::mat4 outlineModel = model * glm::scale(glm::mat4(1.0f), glm::vec3(outlineScale));

							MaterialParams outline = mat;
							outline.baseColor = { 1.0f, 0.72f, 0.10f, 0.95f };
							outline.albedoDescIndex = 0;
							ctx.commandList.SetState(outlineState_);
							DrawOne(mesh, outlineModel, outline);

							MaterialParams hi = mat;
							hi.baseColor = { 1.0f, 0.95f, 0.25f, 0.35f };
							hi.albedoDescIndex = 0; // force solid color
							ctx.commandList.SetState(highlightState_);
							DrawOne(mesh, model, hi);

							ctx.commandList.SetStencilRef(0u);
							ctx.commandList.SetState(state_);
						}
						drewAny = true;
					}

					// If the scene has no draw items or everything is still pending, draw fallback.
					if (!drewAny)
					{
						const glm::mat4 model = glm::rotate(glm::mat4(1.0f), TimeSeconds() * 0.8f, glm::vec3(0, 1, 0));
						MaterialParams mat{};
						mat.baseColor = { 0.2f, 0.3f, 0.7f, 1.0f };
						DrawOne(mesh_, model, mat);
					}
				});

			graph.Execute(device_, swapChain);
			swapChain.Present();
		}

		void Shutdown()
		{
			DestroyMesh(device_, skyboxMesh_);
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
			// Renderer must not perform file IO. Keep fallback procedural.
			cpu.vertices = {
				VertexDesc{-0.8f,-0.6f,0, 0,0,1, 0,0},
				VertexDesc{ 0.8f,-0.6f,0, 0,0,1, 1,0},
				VertexDesc{ 0.0f, 0.9f,0, 0,0,1, 0.5f,1},
			};
			cpu.indices = { 0,1,2 };

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

			// --- Skybox mesh ---
			{
				MeshCPU skyCpu = MakeSkyboxCubeCPU();
				skyboxMesh_ = UploadMesh(device_, skyCpu, "SkyboxCube_GL");
			}

			// --- Skybox shaders/pso ---
			{
				std::filesystem::path vsPath = corefs::ResolveAsset("shaders\\SkyboxVS.vert");
				std::filesystem::path psPath = corefs::ResolveAsset("shaders\\SkyboxFS.frag");

				const auto vs = shaderLibrary_.GetOrCreateShader(ShaderKey{
					.stage = rhi::ShaderStage::Vertex,
					.name = "VS_Skybox_GL",
					.filePath = vsPath.string(),
					.defines = {}
					});

				const auto ps = shaderLibrary_.GetOrCreateShader(ShaderKey{
					.stage = rhi::ShaderStage::Pixel,
					.name = "PS_Skybox_GL",
					.filePath = psPath.string(),
					.defines = {}
					});

				psoSkybox_ = psoCache_.GetOrCreate("PSO_Skybox_GL", vs, ps);

				skyboxState_.depth.testEnable = true;
				skyboxState_.depth.writeEnable = false;
				skyboxState_.depth.depthCompareOp = rhi::CompareOp::LessEqual;

				skyboxState_.rasterizer.cullMode = rhi::CullMode::None;
				skyboxState_.rasterizer.frontFace = rhi::FrontFace::CounterClockwise;

				skyboxState_.blend.enable = false;
			}

			state_.depth.testEnable = true;
			state_.depth.writeEnable = true;
			state_.depth.depthCompareOp = rhi::CompareOp::LessEqual;

			state_.rasterizer.cullMode = rhi::CullMode::None;
			state_.rasterizer.frontFace = rhi::FrontFace::CounterClockwise;

			state_.blend.enable = false;
			highlightState_ = state_;
			highlightState_.blend.enable = true;
			highlightState_.depth.writeEnable = false;

			outlineMarkState_ = state_;
			outlineMarkState_.blend.enable = true;
			outlineMarkState_.depth.writeEnable = false;
			outlineMarkState_.depth.stencil.enable = true;
			outlineMarkState_.depth.stencil.readMask = 0xFFu;
			outlineMarkState_.depth.stencil.writeMask = 0xFFu;
			outlineMarkState_.depth.stencil.front.failOp = rhi::StencilOp::Keep;
			outlineMarkState_.depth.stencil.front.depthFailOp = rhi::StencilOp::Keep;
			outlineMarkState_.depth.stencil.front.passOp = rhi::StencilOp::Replace;
			outlineMarkState_.depth.stencil.front.compareOp = rhi::CompareOp::Always;
			outlineMarkState_.depth.stencil.back = outlineMarkState_.depth.stencil.front;

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
		}

		rhi::IRHIDevice& device_;
		RendererSettings settings_{};

		ShaderLibrary shaderLibrary_;
		PSOCache psoCache_;

		MeshRHI mesh_{}; // fallback-only
		rhi::PipelineHandle psoNoTex_{};
		rhi::PipelineHandle psoTex_{};
		rhi::GraphicsState state_{};
		rhi::GraphicsState highlightState_{};
		rhi::GraphicsState outlineMarkState_{};
		rhi::GraphicsState outlineState_{};

		MeshRHI skyboxMesh_{};
		rhi::PipelineHandle psoSkybox_{};
		rhi::GraphicsState skyboxState_{};

		std::size_t cpuFallbackVertexCount_{ 0 };
	};
} // namespace rendern
