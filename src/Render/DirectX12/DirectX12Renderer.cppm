module;

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
// D3D-style clip-space helpers (Z in [0..1]).
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_access.hpp>

#include <array>
#include <iostream>
#include <algorithm>
#include <filesystem>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>

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
	struct alignas(16) GPULight
	{
		std::array<float, 4> p0{}; // pos.xyz, type
		std::array<float, 4> p1{}; // dir.xyz (FROM light), intensity
		std::array<float, 4> p2{}; // color.rgb, range
		std::array<float, 4> p3{}; // cosInner, cosOuter, attLin, attQuad
	};

	struct alignas(16) InstanceData
	{
		// Column-major 4x4 model matrix (glm::mat4 columns).
		glm::vec4 c0{};
		glm::vec4 c1{};
		glm::vec4 c2{};
		glm::vec4 c3{};
	};

	struct BatchKey
	{
		const rendern::MeshRHI* mesh{};
		// Material key (must be immutable during RenderFrame)
		rhi::TextureDescIndex albedoDescIndex{};
		glm::vec4 baseColor{};
		float shininess{};
		float specStrength{};
		float shadowBias{};
	};

	struct BatchKeyHash
	{
		static std::size_t HashU32(std::uint32_t v) noexcept { return std::hash<std::uint32_t>{}(v); }
		static std::size_t HashPtr(const void* p) noexcept { return std::hash<const void*>{}(p); }

		static std::uint32_t FBits(float v) noexcept
		{
			std::uint32_t b{};
			std::memcpy(&b, &v, sizeof(b));
			return b;
		}

		static void HashCombine(std::size_t& h, std::size_t v) noexcept
		{
			h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
		}

		std::size_t operator()(const BatchKey& k) const noexcept
		{
			std::size_t h = HashPtr(k.mesh);
			HashCombine(h, HashU32((std::uint32_t)k.albedoDescIndex));
			HashCombine(h, HashU32(FBits(k.baseColor.x)));
			HashCombine(h, HashU32(FBits(k.baseColor.y)));
			HashCombine(h, HashU32(FBits(k.baseColor.z)));
			HashCombine(h, HashU32(FBits(k.baseColor.w)));
			HashCombine(h, HashU32(FBits(k.shininess)));
			HashCombine(h, HashU32(FBits(k.specStrength)));
			HashCombine(h, HashU32(FBits(k.shadowBias)));
			return h;
		}
	};

	struct BatchKeyEq
	{
		bool operator()(const BatchKey& a, const BatchKey& b) const noexcept
		{
			return a.mesh == b.mesh &&
				a.albedoDescIndex == b.albedoDescIndex &&
				a.baseColor == b.baseColor &&
				a.shininess == b.shininess &&
				a.specStrength == b.specStrength &&
				a.shadowBias == b.shadowBias;
		}
	};

	struct BatchTemp
	{
		MaterialParams material{};
		std::vector<InstanceData> inst;
	};

	struct Batch
	{
		const rendern::MeshRHI* mesh{};
		MaterialParams material{};
		std::uint32_t instanceOffset = 0; // in instances[]
		std::uint32_t instanceCount = 0;
	};

	struct alignas(16) PerBatchConstants
	{
		std::array<float, 16> uViewProj{};
		std::array<float, 16> uLightViewProj{};
		std::array<float, 4>  uCameraAmbient{}; // xyz + ambient
		std::array<float, 4>  uBaseColor{};
		std::array<float, 4>  uMaterialFlags{}; // shininess, specStrength, shadowBias, flags
		std::array<float, 4>  uCounts{};         // lightCount, ...
	};
	static_assert(sizeof(PerBatchConstants) == 192);

	struct alignas(16) ShadowConstants
	{
		std::array<float, 16> uMVP{}; // lightProj * lightView * model
	};

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

						// --- Instance data (ROWS!) -------------------------------------------------
						struct alignas(16) InstanceData
						{
							glm::vec4 r0{};
							glm::vec4 r1{};
							glm::vec4 r2{};
							glm::vec4 r3{};
						};

						struct ShadowBatch
						{
							const rendern::MeshRHI* mesh{};
							std::uint32_t instanceOffset = 0; // in instances[]
							std::uint32_t instanceCount = 0;
						};

						std::unordered_map<const rendern::MeshRHI*, std::vector<InstanceData>> tmp;
						tmp.reserve(scene.drawItems.size());

						for (const auto& item : scene.drawItems)
						{
							if (!item.mesh || item.mesh->indexCount == 0) continue;

							const glm::mat4 model = item.transform.ToMatrix();

							// HLSL ожидает ROWS в IN.i0..i3, поэтому берём transpose и берём его columns:
							const glm::mat4 mt = glm::transpose(model);

							InstanceData inst{};
							inst.r0 = mt[0];
							inst.r1 = mt[1];
							inst.r2 = mt[2];
							inst.r3 = mt[3];

							tmp[item.mesh].push_back(inst);
						}

						std::vector<InstanceData> instances;
						instances.reserve(scene.drawItems.size());

						std::vector<ShadowBatch> batches;
						batches.reserve(tmp.size());

						for (auto& [mesh, vec] : tmp)
						{
							if (!mesh || vec.empty()) continue;

							ShadowBatch b{};
							b.mesh = mesh;
							b.instanceOffset = static_cast<std::uint32_t>(instances.size());
							b.instanceCount = static_cast<std::uint32_t>(vec.size());

							instances.insert(instances.end(), vec.begin(), vec.end());
							batches.push_back(b);
						}

						if (!instances.empty())
						{
							const std::size_t bytes = instances.size() * sizeof(InstanceData);
							if (bytes > shadowInstanceBufferSizeBytes_)
							{
								throw std::runtime_error("ShadowPass: shadowInstanceBuffer_ overflow (increase shadowInstanceBufferSizeBytes_)");
							}
							device_.UpdateBuffer(shadowInstanceBuffer_, std::as_bytes(std::span{ instances }));
						}

						// --- Per-pass constants (одни на все draw calls) ---------------------------
						struct alignas(16) ShadowConstants
						{
							std::array<float, 16> uLightViewProj{};
						};

						const glm::mat4 lightViewProj = lightProj * lightView;

						ShadowConstants c{};
						std::memcpy(c.uLightViewProj.data(), glm::value_ptr(lightViewProj), sizeof(float) * 16);
						ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &c, 1 }));

						// --- Draw instanced -------------------------------------------------------
						const std::uint32_t instStride = static_cast<std::uint32_t>(sizeof(InstanceData));

						for (const ShadowBatch& b : batches)
						{
							if (!b.mesh || b.instanceCount == 0) continue;

							ctx.commandList.BindInputLayout(b.mesh->layoutInstanced);

							ctx.commandList.BindVertexBuffer(0, b.mesh->vertexBuffer, b.mesh->vertexStrideBytes, 0);
							ctx.commandList.BindVertexBuffer(1, shadowInstanceBuffer_, instStride, b.instanceOffset * instStride);

							ctx.commandList.BindIndexBuffer(b.mesh->indexBuffer, b.mesh->indexType, 0);

							ctx.commandList.DrawIndexed(
								b.mesh->indexCount,
								b.mesh->indexType,
								0, 0,
								b.instanceCount,
								0);
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

					// --- Batch build (proper packing) -------------------------------------------
					// 1) Gather per-batch instance lists
					std::unordered_map<BatchKey, BatchTemp, BatchKeyHash, BatchKeyEq> tmp;
					tmp.reserve(scene.drawItems.size());

					for (const auto& item : scene.drawItems)
					{
						if (!item.mesh || item.mesh->indexCount == 0) continue;

						BatchKey key{};
						key.mesh = item.mesh;

						// IMPORTANT: use the material state as of this frame.
						key.albedoDescIndex = item.material.albedoDescIndex;
						key.baseColor = item.material.baseColor;
						key.shininess = item.material.shininess;
						key.specStrength = item.material.specStrength;
						key.shadowBias = item.material.shadowBias;

						// Build instance data
						const glm::mat4 model = item.transform.ToMatrix();

						InstanceData inst{};
						inst.c0 = glm::row(model, 0);
						inst.c1 = glm::row(model, 1);
						inst.c2 = glm::row(model, 2);
						inst.c3 = glm::row(model, 3);

						auto& bucket = tmp[key];
						if (bucket.inst.empty())
							bucket.material = item.material; // store representative material for this batch
						bucket.inst.push_back(inst);
					}

					// 2) Pack into one big contiguous instances[] + build batches with offsets
					std::vector<InstanceData> instances;
					instances.reserve(scene.drawItems.size());

					std::vector<Batch> batches;
					batches.reserve(tmp.size());

					for (auto& [key, bt] : tmp)
					{
						if (bt.inst.empty())
						{
							continue;
						}

						Batch b{};
						b.mesh = key.mesh;
						b.material = bt.material;
						b.instanceOffset = static_cast<std::uint32_t>(instances.size());
						b.instanceCount = static_cast<std::uint32_t>(bt.inst.size());

						instances.insert(instances.end(), bt.inst.begin(), bt.inst.end());
						batches.push_back(b);
					}

					// 3) Upload instance buffer once
					if (!instances.empty())
					{
						const std::size_t bytes = instances.size() * sizeof(InstanceData);
						if (bytes > instanceBufferSizeBytes_)
						{
							throw std::runtime_error("DX12Renderer: instance buffer overflow (increase instanceBufferSizeBytes_)");
						}

						device_.UpdateBuffer(instanceBuffer_, std::as_bytes(std::span{ instances }));
					}

					// 4) (Optional debug)
					if (settings_.debugPrintDrawCalls)
					{
						static std::uint32_t frame = 0;
						if ((++frame % 60u) == 0u)
						{
							std::cout << "[DX12] MainPass draw calls: " << batches.size()
								<< " (instances total: " << instances.size() << ")\n";
						}
					}
					// --- End batch build --------------------------------------------------------

					const glm::mat4 viewProj = proj * view;
					const glm::mat4 lightViewProj = lightProj * lightView;

					constexpr std::uint32_t kFlagUseTex = 1u << 0;
					constexpr std::uint32_t kFlagUseShadow = 1u << 1;

					for (const Batch& b : batches)
					{
						if (!b.mesh || b.instanceCount == 0)
						{
							continue;
						}
						
						ctx.commandList.BindTextureDesc(0, b.material.albedoDescIndex);

						const bool useTex = (b.material.albedoDescIndex != 0);
						std::uint32_t flags = 0;
						if (useTex)
						{
							flags |= kFlagUseTex;
						}
						flags |= kFlagUseShadow; // shadow map already bound at slot 1

						// --- constants ---
						PerBatchConstants constatntsBatch{};
						std::memcpy(constatntsBatch.uViewProj.data(), glm::value_ptr(viewProj), sizeof(float) * 16);
						std::memcpy(constatntsBatch.uLightViewProj.data(), glm::value_ptr(lightViewProj), sizeof(float) * 16);

						constatntsBatch.uCameraAmbient = { camPos.x, camPos.y, camPos.z, 0.22f };
						constatntsBatch.uBaseColor = { b.material.baseColor.x, b.material.baseColor.y, b.material.baseColor.z, b.material.baseColor.w };

						const float shadowBias = (b.material.shadowBias != 0.0f) ? b.material.shadowBias : 0.0015f;
						constatntsBatch.uMaterialFlags = { b.material.shininess, b.material.specStrength, shadowBias, AsFloatBits(flags) };
						constatntsBatch.uCounts = { (float)lightCount, 0, 0, 0 };

						// --- IA (instanced) ---
						ctx.commandList.BindInputLayout(b.mesh->layoutInstanced);
						ctx.commandList.BindVertexBuffer(0, b.mesh->vertexBuffer, b.mesh->vertexStrideBytes, 0);

						const std::uint32_t instStride = (std::uint32_t)sizeof(InstanceData);
						const std::uint32_t instOffsetBytes = b.instanceOffset * instStride;
						ctx.commandList.BindVertexBuffer(1, instanceBuffer_, instStride, instOffsetBytes);

						ctx.commandList.BindIndexBuffer(b.mesh->indexBuffer, b.mesh->indexType, 0);

						// bind constants to root param 0 (as before)
						ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &constatntsBatch, 1 }));
						ctx.commandList.DrawIndexed(b.mesh->indexCount, b.mesh->indexType, 0, 0, b.instanceCount, 0);
					}
				});

			graph.Execute(device_, swapChain);
			swapChain.Present();
		}

		void Shutdown()
		{
			if (instanceBuffer_)
			{
				device_.DestroyBuffer(instanceBuffer_);
			}
			if (shadowInstanceBuffer_)
			{
				device_.DestroyBuffer(shadowInstanceBuffer_);
			}
			if (lightsBuffer_)
			{
				device_.DestroyBuffer(lightsBuffer_);
			}
			psoCache_.ClearCache();
			shaderLibrary_.ClearCache();
		}

	private:
		static float AsFloatBits(std::uint32_t u) noexcept
		{
			float f{};
			std::memcpy(&f, &u, sizeof(u));
			return f;
		}

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
				shaderPath = corefs::ResolveAsset("shaders\\GlobalShaderInstanced_dx12.hlsl");
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

			// DX12-only dynamic buffers
			if (device_.GetBackend() == rhi::Backend::DirectX12)
			{
				// Lights structured buffer (t2)
				{
					rhi::BufferDesc ld{};
					ld.bindFlag = rhi::BufferBindFlag::StructuredBuffer;
					ld.usageFlag = rhi::BufferUsageFlag::Dynamic;
					ld.sizeInBytes = sizeof(GPULight) * kMaxLights;
					ld.structuredStrideBytes = static_cast<std::uint32_t>(sizeof(GPULight));
					ld.debugName = "LightsSB";
					lightsBuffer_ = device_.CreateBuffer(ld);
				}

				// Per-instance model matrices VB (slot1)
				{
					rhi::BufferDesc id{};
					id.bindFlag = rhi::BufferBindFlag::VertexBuffer;
					id.usageFlag = rhi::BufferUsageFlag::Dynamic;
					id.sizeInBytes = instanceBufferSizeBytes_;
					id.debugName = "InstanceVB";
					instanceBuffer_ = device_.CreateBuffer(id);
				}
				// Per-instance model matrices for ShadowPass (slot1)
				{
					rhi::BufferDesc id{};
					id.bindFlag = rhi::BufferBindFlag::VertexBuffer;
					id.usageFlag = rhi::BufferUsageFlag::Dynamic;
					id.sizeInBytes = shadowInstanceBufferSizeBytes_;
					id.debugName = "ShadowInstanceVB";
					shadowInstanceBuffer_ = device_.CreateBuffer(id);
				}
			}
		}

	private:
		static constexpr std::uint32_t kMaxLights = 64;
		static constexpr std::uint32_t kDefaultInstanceBufferSizeBytes = 1024u * 1024u; // 1 MB (~16k instances)

		rhi::IRHIDevice& device_;
		RendererSettings settings_{};

		ShaderLibrary shaderLibrary_;
		PSOCache psoCache_;

		// Main pass
		rhi::PipelineHandle pso_{};
		rhi::GraphicsState state_{};

		rhi::BufferHandle instanceBuffer_{};
		std::uint32_t instanceBufferSizeBytes_{ kDefaultInstanceBufferSizeBytes };

		rhi::BufferHandle shadowInstanceBuffer_{};
		std::uint32_t shadowInstanceBufferSizeBytes_{ kDefaultInstanceBufferSizeBytes };

		// Shadow pass
		rhi::PipelineHandle psoShadow_{};
		rhi::GraphicsState shadowState_{};

		rhi::BufferHandle lightsBuffer_{};
	};
} // namespace rendern
