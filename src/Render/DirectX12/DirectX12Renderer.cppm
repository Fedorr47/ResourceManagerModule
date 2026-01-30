module;

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
// D3D-style clip-space helpers (Z in [0..1]).
#include <glm/ext/matrix_clip_space.hpp>

#include <array>
#include <algorithm>
#include <filesystem>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

export module core:renderer_dx12;

import :rhi;
import :scene;
import :renderer_settings;
import :render_core;
import :render_graph;
import :file_system;
import :mesh;

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

		void RenderFrame(rhi::IRHISwapChain& swapChain, const Scene& scene)
		{
			renderGraph::RenderGraph graph;

			// ---------------- Shadow map (directional) ----------------
			const rhi::Extent2D shadowExtent{ 2048, 2048 };
			const auto shadowRG = graph.CreateTexture(renderGraph::RGTextureDesc{
				.extent = shadowExtent,
				.format = rhi::Format::D32_FLOAT,
				.usage = renderGraph::ResourceUsage::DepthStencil,
				.debugName = "ShadowMap"
			});

			// Choose first directional light (or a default).
			glm::vec3 lightDir = glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f)); // FROM light towards scene
			for (const auto& l : scene.lights)
			{
				if (l.type == LightType::Directional)
				{
					lightDir = glm::normalize(l.direction);
					break;
				}
			}

			const glm::vec3 center = scene.camera.target; // stage-1 heuristic
			const float lightDist = 10.0f;
			const glm::vec3 lightPos = center - lightDir * lightDist;
			const glm::mat4 lightView = glm::lookAt(lightPos, center, glm::vec3(0, 1, 0));

			// Stage-1: fixed ortho volume around the origin/target.
			const float orthoHalf = 8.0f;
			const glm::mat4 lightProj = glm::orthoRH_ZO(-orthoHalf, orthoHalf, -orthoHalf, orthoHalf, 0.1f, 40.0f);

			// Shadow pass (depth-only)
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
					[this, &scene, lightView, lightProj](renderGraph::PassContext& ctx)
					{
						ctx.commandList.SetViewport(0, 0,
							static_cast<int>(ctx.passExtent.width),
							static_cast<int>(ctx.passExtent.height));

						ctx.commandList.SetState(shadowState_);
						ctx.commandList.BindPipeline(psoShadow_);

						struct alignas(16) ShadowConstants
						{
							std::array<float, 16> uMVP{}; // lightProj * lightView * model
						};

						for (const auto& item : scene.drawItems)
						{
							if (!item.mesh || item.mesh->indexCount == 0)
								continue;

							const glm::mat4 model = item.transform.ToMatrix();
							const glm::mat4 mvp = lightProj * lightView * model;

							ShadowConstants c{};
							std::memcpy(c.uMVP.data(), glm::value_ptr(mvp), sizeof(float) * 16);

							ctx.commandList.BindInputLayout(item.mesh->layout);
							ctx.commandList.BindVertexBuffer(0, item.mesh->vertexBuffer, item.mesh->vertexStrideBytes, 0);
							ctx.commandList.BindIndexBuffer(item.mesh->indexBuffer, item.mesh->indexType, 0);

							ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &c, 1 }));
							ctx.commandList.DrawIndexed(item.mesh->indexCount, item.mesh->indexType, 0, 0);
						}
					});
			}

			// ---------------- Main pass (swapchain) ----------------
			rhi::ClearDesc clearDesc{};
			clearDesc.clearColor = true;
			clearDesc.clearDepth = true;
			clearDesc.color = { 0.1f, 0.1f, 0.1f, 1.0f };

			graph.AddSwapChainPass("MainPass", clearDesc,
				[this, &scene, shadowRG, lightView, lightProj](renderGraph::PassContext& ctx)
				{
					const auto extent = ctx.passExtent;

					ctx.commandList.SetViewport(0, 0,
						static_cast<int>(extent.width),
						static_cast<int>(extent.height));

					ctx.commandList.SetState(state_);
					ctx.commandList.BindPipeline(pso_);

					// Bind shadow map at slot 1 (t1)
					{
						const auto shadowTex = ctx.resources.GetTexture(shadowRG);
						ctx.commandList.BindTexture2D(1, shadowTex);
					}

					const float aspect = extent.height
						? (static_cast<float>(extent.width) / static_cast<float>(extent.height))
						: 1.0f;

					const glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(scene.camera.fovYDeg), aspect, scene.camera.nearZ, scene.camera.farZ);
					const glm::mat4 view = glm::lookAt(scene.camera.position, scene.camera.target, scene.camera.up);
					const glm::vec3 camPos = scene.camera.position;

					// Upload and bind lights (t2 StructuredBuffer SRV)
					const std::uint32_t lightCount = UploadLights(scene, camPos);
					ctx.commandList.BindStructuredBufferSRV(2, lightsBuffer_);

					struct alignas(16) PerDrawConstants
					{
						std::array<float, 16> uMVP{};       // 64
						std::array<float, 16> uLightMVP{};  // 64
						std::array<float, 12> uModelRows{}; // 48 (row0..row2)
						std::array<float, 4>  uCameraAmbient{};  // cam.xyz, ambientStrength
						std::array<float, 4>  uBaseColor{};      // rgba
						std::array<float, 4>  uMaterialFlags{};  // shininess, specStrength, shadowBias, flags(float)
						std::array<float, 4>  uCounts{};         // x = lightCount
					};
					static_assert(sizeof(PerDrawConstants) == 240);

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

					for (const auto& item : scene.drawItems)
					{
						if (!item.mesh || item.mesh->indexCount == 0)
							continue;

						// Bind albedo at slot 0 (t0) by descriptor index (0 = null SRV)
						ctx.commandList.BindTextureDesc(0, item.material.albedoDescIndex);

						const bool useTex = (item.material.albedoDescIndex != 0);
						std::uint32_t flags = 0;
						if (useTex) flags |= kFlagUseTex;
						flags |= kFlagUseShadow;

						const glm::mat4 model = item.transform.ToMatrix();
						const glm::mat4 mvp = proj * view * model;
						const glm::mat4 lightMVP = lightProj * lightView * model;

						PerDrawConstants c{};
						std::memcpy(c.uMVP.data(), glm::value_ptr(mvp), sizeof(float) * 16);
						std::memcpy(c.uLightMVP.data(), glm::value_ptr(lightMVP), sizeof(float) * 16);

						WriteRow(model, 0, c.uModelRows, 0);
						WriteRow(model, 1, c.uModelRows, 1);
						WriteRow(model, 2, c.uModelRows, 2);

						c.uCameraAmbient = { camPos.x, camPos.y, camPos.z, 0.22f };
						c.uBaseColor = { item.material.baseColor.x, item.material.baseColor.y, item.material.baseColor.z, item.material.baseColor.w };

						const float shininess = item.material.shininess;
						const float specStrength = item.material.specStrength;
						const float shadowBias = (item.material.shadowBias != 0.0f) ? item.material.shadowBias : 0.0015f;

						c.uMaterialFlags = { shininess, specStrength, shadowBias, static_cast<float>(flags) };
						c.uCounts = { static_cast<float>(lightCount), 0.0f, 0.0f, 0.0f };

						ctx.commandList.BindInputLayout(item.mesh->layout);
						ctx.commandList.BindVertexBuffer(0, item.mesh->vertexBuffer, item.mesh->vertexStrideBytes, 0);
						ctx.commandList.BindIndexBuffer(item.mesh->indexBuffer, item.mesh->indexType, 0);

						ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &c, 1 }));
						ctx.commandList.DrawIndexed(item.mesh->indexCount, item.mesh->indexType, 0, 0);
					}
				});

			graph.Execute(device_, swapChain);
			swapChain.Present();
		}

		void Shutdown()
		{
			if (lightsBuffer_)
			{
				device_.DestroyBuffer(lightsBuffer_);
			}
			psoCache_.ClearCache();
			shaderLibrary_.ClearCache();
		}

	private:
		static constexpr std::uint32_t kMaxLights = 64;

		struct alignas(16) GPULight
		{
			std::array<float, 4> p0{}; // pos.xyz, type
			std::array<float, 4> p1{}; // dir.xyz (FROM light), intensity
			std::array<float, 4> p2{}; // color.rgb, range
			std::array<float, 4> p3{}; // cosInner, cosOuter, attLin, attQuad
		};

		std::uint32_t UploadLights(const Scene& scene, const glm::vec3& camPos)
		{
			std::vector<GPULight> gpu;
			gpu.reserve(std::min<std::size_t>(scene.lights.size(), kMaxLights));

			for (const auto& l : scene.lights)
			{
				if (gpu.size() >= kMaxLights)
					break;

				GPULight out{};

				out.p0 = { l.position.x, l.position.y, l.position.z, static_cast<float>(static_cast<std::uint32_t>(l.type)) };
				out.p1 = { l.direction.x, l.direction.y, l.direction.z, l.intensity };
				out.p2 = { l.color.x, l.color.y, l.color.z, l.range };

				const float cosOuter = std::cos(glm::radians(l.outerAngleDeg));
				const float cosInner = std::cos(glm::radians(l.innerAngleDeg));

				out.p3 = { cosInner, cosOuter, l.attLinear, l.attQuadratic };
				gpu.push_back(out);
			}

			// Small default rig if the scene didn't provide any lights
			if (gpu.empty())
			{
				GPULight dir{};
				const glm::vec3 dirFromLight = glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f));
				dir.p0 = { 0,0,0, static_cast<float>(static_cast<std::uint32_t>(LightType::Directional)) };
				dir.p1 = { dirFromLight.x, dirFromLight.y, dirFromLight.z, 1.2f };
				dir.p2 = { 1.0f, 1.0f, 1.0f, 0.0f };
				dir.p3 = { 0,0,0,0 };
				gpu.push_back(dir);

				GPULight point{};
				point.p0 = { 2.5f, 2.0f, 1.5f, static_cast<float>(static_cast<std::uint32_t>(LightType::Point)) };
				point.p1 = { 0,0,0, 2.0f };
				point.p2 = { 1.0f, 0.95f, 0.8f, 12.0f };
				point.p3 = { 0,0, 0.12f, 0.04f };
				gpu.push_back(point);

				GPULight spot{};
				const glm::vec3 spotPos = camPos;
				const glm::vec3 spotDir = glm::normalize(glm::vec3(0, 0, 0) - camPos);
				spot.p0 = { spotPos.x, spotPos.y, spotPos.z, static_cast<float>(static_cast<std::uint32_t>(LightType::Spot)) };
				spot.p1 = { spotDir.x, spotDir.y, spotDir.z, 3.0f };
				spot.p2 = { 0.8f, 0.9f, 1.0f, 30.0f };
				spot.p3 = { std::cos(glm::radians(12.0f)), std::cos(glm::radians(20.0f)), 0.09f, 0.032f };
				gpu.push_back(spot);
			}

			device_.UpdateBuffer(lightsBuffer_, std::as_bytes(std::span{ gpu }));
			return static_cast<std::uint32_t>(gpu.size());
		}

		void CreateResources()
		{
			std::filesystem::path shaderPath;
			std::filesystem::path shadowPath;

			switch (device_.GetBackend())
			{
			case rhi::Backend::DirectX12:
				shaderPath = corefs::ResolveAsset("shaders\\GlobalShader_dx12.hlsl");
				shadowPath = corefs::ResolveAsset("shaders\\Shadow_dx12.hlsl");
				break;
			default:
				shaderPath = corefs::ResolveAsset("shaders\\VS.vert");
				shadowPath = corefs::ResolveAsset("shaders\\VS.vert");
				break;
			}

			// Main pipeline
			{
				const auto vs = shaderLibrary_.GetOrCreateShader(ShaderKey{
					.stage = rhi::ShaderStage::Vertex,
					.name = "VS_Mesh",
					.filePath = shaderPath.string(),
					.defines = {}
				});
				const auto ps = shaderLibrary_.GetOrCreateShader(ShaderKey{
					.stage = rhi::ShaderStage::Pixel,
					.name = "PS_Mesh",
					.filePath = shaderPath.string(),
					.defines = {}
				});

				pso_ = psoCache_.GetOrCreate("PSO_Mesh", vs, ps);

				state_.depth.testEnable = true;
				state_.depth.writeEnable = true;
				state_.depth.depthCompareOp = rhi::CompareOp::LessEqual;

				state_.rasterizer.cullMode = rhi::CullMode::Back;
				state_.rasterizer.frontFace = rhi::FrontFace::CounterClockwise;

				state_.blend.enable = false;
			}

			// Shadow pipeline (depth-only)
			if (device_.GetBackend() == rhi::Backend::DirectX12)
			{
				const auto vsShadow = shaderLibrary_.GetOrCreateShader(ShaderKey{
					.stage = rhi::ShaderStage::Vertex,
					.name = "VS_Shadow",
					.filePath = shadowPath.string(),
					.defines = {}
				});
				const auto psShadow = shaderLibrary_.GetOrCreateShader(ShaderKey{
					.stage = rhi::ShaderStage::Pixel,
					.name = "PS_Shadow",
					.filePath = shadowPath.string(),
					.defines = {}
				});

				psoShadow_ = psoCache_.GetOrCreate("PSO_Shadow", vsShadow, psShadow);

				shadowState_.depth.testEnable = true;
				shadowState_.depth.writeEnable = true;
				shadowState_.depth.depthCompareOp = rhi::CompareOp::LessEqual;

				// Disable culling for the shadow pass in stage-1 (avoid winding issues).
				shadowState_.rasterizer.cullMode = rhi::CullMode::None;
				shadowState_.rasterizer.frontFace = rhi::FrontFace::CounterClockwise;

				shadowState_.blend.enable = false;
			}

			// Lights structured buffer (SRV)
			if (device_.GetBackend() == rhi::Backend::DirectX12)
			{
				rhi::BufferDesc ld{};
				ld.bindFlag = rhi::BufferBindFlag::StructuredBuffer;
				ld.usageFlag = rhi::BufferUsageFlag::Dynamic;
				ld.sizeInBytes = sizeof(GPULight) * kMaxLights;
				ld.structuredStrideBytes = static_cast<std::uint32_t>(sizeof(GPULight));
				ld.debugName = "LightsSB";

				lightsBuffer_ = device_.CreateBuffer(ld);
			}
		}

	private:

		rhi::IRHIDevice& device_;
		RendererSettings settings_{};

		ShaderLibrary shaderLibrary_;
		PSOCache psoCache_;

		// Main pass
		rhi::PipelineHandle pso_{};
		rhi::GraphicsState state_{};

		// Shadow pass
		rhi::PipelineHandle psoShadow_{};
		rhi::GraphicsState shadowState_{};

		rhi::BufferHandle lightsBuffer_{};
	};
} // namespace rendern
