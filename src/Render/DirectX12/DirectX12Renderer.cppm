module;

#if defined(_WIN32)
// Prevent Windows headers from defining the `min`/`max` macros which break `std::min`/`std::max`.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#endif


// D3D-style clip-space helpers (Z in [0..1]).

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
import :math_utils;
import :renderer_settings;
import :render_core;
import :render_graph;
import :file_system;
import :mesh;
import :scene_bridge;

export namespace rendern
{

	// multiple Spot/Point shadow casters (DX12). Keep small caps for now.
	constexpr std::uint32_t kMaxSpotShadows = 4;
	constexpr std::uint32_t kMaxPointShadows = 4;

	struct alignas(16) GPULight
	{
		std::array<float, 4> p0{}; // pos.xyz, type
		std::array<float, 4> p1{}; // dir.xyz (FROM light), intensity
		std::array<float, 4> p2{}; // color.rgb, range
		std::array<float, 4> p3{}; // cosInner, cosOuter, attLin, attQuad
	};

	struct InstanceData
	{
		mathUtils::Vec4 i0; // column 0 of model
		mathUtils::Vec4 i1; // column 1
		mathUtils::Vec4 i2; // column 2
		mathUtils::Vec4 i3; // column 3
	};
	static_assert(sizeof(InstanceData) == 64);

	struct BatchKey
	{
		const rendern::MeshRHI* mesh{};
		// Material key (must be immutable during RenderFrame)
		rhi::TextureDescIndex albedoDescIndex{};
		mathUtils::Vec4 baseColor{};
		float shininess{};
		float specStrength{};
		float shadowBias{};
		rendern::MaterialHandle material{};
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
		MaterialHandle materialHandle{};
		std::vector<InstanceData> inst;
	};

	struct Batch
	{
		const rendern::MeshRHI* mesh{};
		MaterialParams material{};
		MaterialHandle materialHandle{};
		std::uint32_t instanceOffset = 0; // in instances[]
		std::uint32_t instanceCount = 0;
	};

		struct alignas(16) PerBatchConstants
	{
		std::array<float, 16> uViewProj{};
		std::array<float, 16> uLightViewProj{};
		std::array<float, 4>  uCameraAmbient{}; // xyz + ambient
		std::array<float, 4>  uBaseColor{};     // fallback baseColor

		// shininess, specStrength, materialShadowBiasTexels, flagsBits
		std::array<float, 4>  uMaterialFlags{};

		// lightCount, spotShadowCount, pointShadowCount, unused
		std::array<float, 4>  uCounts{};

		// dirBaseTexels, spotBaseTexels, pointBaseTexels, slopeScaleTexels
		std::array<float, 4>  uShadowBias{};
	};
	static_assert(sizeof(PerBatchConstants) == 208);


	// shadow metadata for Spot/Point arrays (bound as StructuredBuffer at t11).
	// We pack indices/bias as floats to keep the struct simple across compilers.
	struct alignas(16) ShadowDataSB
	{
		// Spot view-projection matrices as ROWS (4 matrices * 4 rows = 16 float4).
		std::array<mathUtils::Vec4, kMaxSpotShadows * 4> spotVPRows{};
		// spotInfo[i] = { lightIndexBits, bias, 0, 0 }
		std::array<mathUtils::Vec4, kMaxSpotShadows>     spotInfo{};

		// pointPosRange[i] = { pos.x, pos.y, pos.z, range }
		std::array<mathUtils::Vec4, kMaxPointShadows>    pointPosRange{};
		// pointInfo[i] = { lightIndexBits, bias, 0, 0 }
		std::array<mathUtils::Vec4, kMaxPointShadows>    pointInfo{};
	};
	static_assert((sizeof(ShadowDataSB) % 16) == 0);

	// ---------------- Spot/Point shadow maps (arrays) ----------------
	struct SpotShadowRec
	{
		renderGraph::RGTextureHandle tex{};
		mathUtils::Mat4 viewProj{};
		std::uint32_t lightIndex = 0;
	};

	struct PointShadowRec
	{
		renderGraph::RGTextureHandle cube{};
		renderGraph::RGTextureHandle depthTmp{};
		mathUtils::Vec3 pos{};
		float range = 10.0f;
		std::uint32_t lightIndex = 0;
	};

	struct alignas(16) ShadowConstants
	{
		std::array<float, 16> uMVP{}; // lightProj * lightView * model
	};

	struct ShadowBatch
	{
		const rendern::MeshRHI* mesh{};
		std::uint32_t instanceOffset = 0; // in combinedInstances[]
		std::uint32_t instanceCount = 0;
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

			// -------------------------------------------------------------------------
			// IMPORTANT (DX12): UpdateBuffer() is flushed at the beginning of SubmitCommandList().
			// Therefore, all UpdateBuffer() calls MUST happen before graph.Execute().
			// -------------------------------------------------------------------------

			// --- camera (used for fallback lights too) ---
			const mathUtils::Vec3 camPos = scene.camera.position;

			// Upload lights once per frame (t2 StructuredBuffer SRV)
			const std::uint32_t lightCount = UploadLights(scene, camPos);

			// ---------------- Directional shadow (single) ----------------
			const rhi::Extent2D shadowExtent{ 2048, 2048 };
			const auto shadowRG = graph.CreateTexture(renderGraph::RGTextureDesc{
				.extent = shadowExtent,
				.format = rhi::Format::D32_FLOAT,
				.usage = renderGraph::ResourceUsage::DepthStencil,
				.debugName = "ShadowMap"
			});

			// Choose first directional light (or a default).
			mathUtils::Vec3 lightDir = mathUtils::Normalize(mathUtils::Vec3(-0.4f, -1.0f, -0.3f)); // FROM light towards scene
			for (const auto& l : scene.lights)
			{
				if (l.type == LightType::Directional)
				{
					lightDir = mathUtils::Normalize(l.direction);
					break;
				}
			}

			const mathUtils::Vec3 center = scene.camera.target;
			const float lightDist = 10.0f;
			const mathUtils::Vec3 lightPos = center - lightDir * lightDist;
			const mathUtils::Mat4 lightView = mathUtils::LookAt(lightPos, center, mathUtils::Vec3(0, 1, 0));

			const float orthoHalf = 8.0f;
			const mathUtils::Mat4 lightProj = mathUtils::OrthoRH_ZO(-orthoHalf, orthoHalf, -orthoHalf, orthoHalf, 0.1f, 40.0f);
			const mathUtils::Mat4 dirLightViewProj = lightProj * lightView;

			std::vector<SpotShadowRec> spotShadows;
			std::vector<PointShadowRec> pointShadows;
			spotShadows.reserve(kMaxSpotShadows);
			pointShadows.reserve(kMaxPointShadows);


			// ---------------- Build instance draw lists (ONE upload) ----------------
			// We build two packings:
			//   1) Shadow packing: per-mesh batching (used by directional/spot/point shadow passes)
			//   2) Main packing: per-(mesh+material params) batching (used by MainPass)
			//
			// Then we concatenate them into a single instanceBuffer_ update.
			// ---- Shadow packing (per mesh) ----
			std::unordered_map<const rendern::MeshRHI*, std::vector<InstanceData>> shadowTmp;
			shadowTmp.reserve(scene.drawItems.size());

			for (const auto& item : scene.drawItems)
			{
				const rendern::MeshRHI* mesh = item.mesh ? &item.mesh->GetResource() : nullptr;
				if (!mesh || mesh->indexCount == 0)
				{
					continue;
				}

				const mathUtils::Mat4 model = item.transform.ToMatrix();

				InstanceData inst{};
				inst.i0 = model[0];
				inst.i1 = model[1];
				inst.i2 = model[2];
				inst.i3 = model[3];

				shadowTmp[mesh].push_back(inst);
			}

			std::vector<InstanceData> shadowInstances;
			std::vector<ShadowBatch> shadowBatches;
			shadowInstances.reserve(scene.drawItems.size());
			shadowBatches.reserve(shadowTmp.size());

			{
				std::vector<const rendern::MeshRHI*> meshes;
				meshes.reserve(shadowTmp.size());
				for (auto& [shadowMesh, _] : shadowTmp)
				{
					meshes.push_back(shadowMesh);
				}
				std::sort(meshes.begin(), meshes.end());

				for (const rendern::MeshRHI* mesh : meshes)
				{
					auto& vec = shadowTmp[mesh];
					if (!mesh || vec.empty()) continue;

					ShadowBatch shadowBatch{};
					shadowBatch.mesh = mesh;
					shadowBatch.instanceOffset = static_cast<std::uint32_t>(shadowInstances.size());
					shadowBatch.instanceCount = static_cast<std::uint32_t>(vec.size());

					shadowInstances.insert(shadowInstances.end(), vec.begin(), vec.end());
					shadowBatches.push_back(shadowBatch);
				}
			}

			// ---- Main packing (per mesh + material params) ----
			std::unordered_map<BatchKey, BatchTemp, BatchKeyHash, BatchKeyEq> mainTmp;
			mainTmp.reserve(scene.drawItems.size());

			for (const auto& item : scene.drawItems)
			{
				const rendern::MeshRHI* mesh = item.mesh ? &item.mesh->GetResource() : nullptr;
				if (!mesh || mesh->indexCount == 0)
				{
					continue;
				}

				BatchKey key{};
				key.mesh = mesh;
				key.material = item.material;

				MaterialParams params{};
				if (item.material.id != 0)
				{
					params = scene.GetMaterial(item.material).params;
				}

				// IMPORTANT: BatchKey must include material parameters,
				// otherwise different materials get incorrectly merged.
				key.albedoDescIndex = params.albedoDescIndex;
				key.baseColor = params.baseColor;
				key.shininess = params.shininess;
				key.specStrength = params.specStrength;
				key.shadowBias = params.shadowBias; // texels

				// Instance (ROWS)
				const mathUtils::Mat4 model = item.transform.ToMatrix();

				InstanceData inst{};
				inst.i0 = model[0];
				inst.i1 = model[1];
				inst.i2 = model[2];
				inst.i3 = model[3];

				auto& bucket = mainTmp[key];
				if (bucket.inst.empty())
				{
					bucket.materialHandle = item.material;
					bucket.material = params; // representative material for this batch
				}
				bucket.inst.push_back(inst);
			}

			std::vector<InstanceData> mainInstances;
			mainInstances.reserve(scene.drawItems.size());

			std::vector<Batch> mainBatches;
			mainBatches.reserve(mainTmp.size());

			for (auto& [key, bt] : mainTmp)
			{
				if (bt.inst.empty()) continue;

				Batch b{};
				b.mesh = key.mesh;
				b.materialHandle = bt.materialHandle;
				b.material = bt.material;
				b.instanceOffset = static_cast<std::uint32_t>(mainInstances.size());
				b.instanceCount = static_cast<std::uint32_t>(bt.inst.size());

				mainInstances.insert(mainInstances.end(), bt.inst.begin(), bt.inst.end());
				mainBatches.push_back(b);
			}

			// ---- Combine and upload once ----
			const std::uint32_t shadowBase = 0;
			const std::uint32_t mainBase = static_cast<std::uint32_t>(shadowInstances.size());

			for (auto& sbatch : shadowBatches)
			{
				sbatch.instanceOffset += shadowBase;
			}
			for (auto& mbatch : mainBatches)
			{
				mbatch.instanceOffset += mainBase;
			}

			std::vector<InstanceData> combinedInstances;
			combinedInstances.reserve(shadowInstances.size() + mainInstances.size());
			combinedInstances.insert(combinedInstances.end(), shadowInstances.begin(), shadowInstances.end());
			combinedInstances.insert(combinedInstances.end(), mainInstances.begin(), mainInstances.end());

			const std::uint32_t instStride = static_cast<std::uint32_t>(sizeof(InstanceData));

			if (!combinedInstances.empty())
			{
				const std::size_t bytes = combinedInstances.size() * sizeof(InstanceData);
				if (bytes > instanceBufferSizeBytes_)
				{
					throw std::runtime_error("DX12Renderer: instance buffer overflow (increase instanceBufferSizeBytes_)");
				}
				device_.UpdateBuffer(instanceBuffer_, std::as_bytes(std::span{ combinedInstances }));
			}

			if (settings_.debugPrintDrawCalls)
			{
				static std::uint32_t frame = 0;
				if ((++frame % 60u) == 0u)
				{
					std::cout << "[DX12] MainPass draw calls: " << mainBatches.size()
						<< " (instances main: " << mainInstances.size()
						<< ", shadow: " << shadowInstances.size() << ")\n";
				}
			}

			// ---------------- Create shadow passes (all reuse shadowBatches) ----------------
			// Shadow pass (directional, depth-only)
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

				// Pre-pack constants once (per pass)
				struct alignas(16) ShadowPassConstants
				{
					std::array<float, 16> uLightViewProj{};
				};

				ShadowPassConstants c{};
				const mathUtils::Mat4 dirVP_T = mathUtils::Transpose(dirLightViewProj);
				std::memcpy(c.uLightViewProj.data(), mathUtils::ValuePtr(dirVP_T), sizeof(float) * 16);

				graph.AddPass("ShadowPass", std::move(att),
					[this, c, shadowBatches, instStride](renderGraph::PassContext& ctx) mutable
					{
						ctx.commandList.SetViewport(0, 0,
							static_cast<int>(ctx.passExtent.width),
							static_cast<int>(ctx.passExtent.height));

						ctx.commandList.SetState(shadowState_);
						ctx.commandList.BindPipeline(psoShadow_);

						ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &c, 1 }));

						for (const ShadowBatch& b : shadowBatches)
						{
							if (!b.mesh || b.instanceCount == 0) continue;

							ctx.commandList.BindInputLayout(b.mesh->layoutInstanced);
							ctx.commandList.BindVertexBuffer(0, b.mesh->vertexBuffer, b.mesh->vertexStrideBytes, 0);
							ctx.commandList.BindVertexBuffer(1, instanceBuffer_, instStride, b.instanceOffset * instStride);
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

			// Collect up to kMaxSpotShadows / kMaxPointShadows from scene.lights (index aligns with UploadLights()).
			for (std::uint32_t li = 0; li < static_cast<std::uint32_t>(scene.lights.size()); ++li)
			{
				if (li >= kMaxLights)
					break;

				const auto& l = scene.lights[li];

				if (l.type == LightType::Spot && spotShadows.size() < kMaxSpotShadows)
				{
					const rhi::Extent2D ext{ 1024, 1024 };
					const auto rg = graph.CreateTexture(renderGraph::RGTextureDesc{
						.extent = ext,
						.format = rhi::Format::D32_FLOAT,
						.usage = renderGraph::ResourceUsage::DepthStencil,
						.debugName = "SpotShadowMap"
					});

					mathUtils::Vec3 dir = mathUtils::Normalize(l.direction);
					mathUtils::Vec3 up = (std::abs(mathUtils::Dot(dir, mathUtils::Vec3(0, 1, 0))) > 0.99f)
						? mathUtils::Vec3(0, 0, 1)
						: mathUtils::Vec3(0, 1, 0);

					mathUtils::Mat4 v = mathUtils::LookAt(l.position, l.position + dir, up);

					const float outerHalf = std::max(1.0f, l.outerHalfAngleDeg);
					const float farZ = std::max(1.0f, l.range);
					const float nearZ = std::max(0.5f, farZ * 0.02f);
					const mathUtils::Mat4 p = mathUtils::PerspectiveRH_ZO(mathUtils::ToRadians(outerHalf * 2.0f), 1.0f, nearZ, farZ);
					const mathUtils::Mat4 vp = p * v;

					SpotShadowRec rec{};
					rec.tex = rg;
					rec.viewProj = vp;
					rec.lightIndex = li;
					spotShadows.push_back(rec);

					rhi::ClearDesc clear{};
					clear.clearColor = false;
					clear.clearDepth = true;
					clear.depth = 1.0f;

					renderGraph::PassAttachments att{};
					att.useSwapChainBackbuffer = false;
					att.color = std::nullopt;
					att.depth = rg;
					att.clearDesc = clear;

					const std::string passName = "SpotShadowPass_" + std::to_string(static_cast<int>(spotShadows.size() - 1));

					// Pre-pack constants once (per pass)
					struct alignas(16) SpotPassConstants
					{
						std::array<float, 16> uLightViewProj{};
					};
					SpotPassConstants c{};
					const mathUtils::Mat4 vpT = mathUtils::Transpose(vp);
					std::memcpy(c.uLightViewProj.data(), mathUtils::ValuePtr(vpT), sizeof(float) * 16);

					graph.AddPass(passName, std::move(att),
						[this, c, shadowBatches, instStride](renderGraph::PassContext& ctx) mutable
						{
							ctx.commandList.SetViewport(0, 0,
								static_cast<int>(ctx.passExtent.width),
								static_cast<int>(ctx.passExtent.height));

							ctx.commandList.SetState(shadowState_);
							ctx.commandList.BindPipeline(psoShadow_);

							ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &c, 1 }));

							for (const ShadowBatch& b : shadowBatches)
							{
								if (!b.mesh || b.instanceCount == 0) continue;

								ctx.commandList.BindInputLayout(b.mesh->layoutInstanced);

								ctx.commandList.BindVertexBuffer(0, b.mesh->vertexBuffer, b.mesh->vertexStrideBytes, 0);
								ctx.commandList.BindVertexBuffer(1, instanceBuffer_, instStride, b.instanceOffset * instStride);

								ctx.commandList.BindIndexBuffer(b.mesh->indexBuffer, b.mesh->indexType, 0);

								ctx.commandList.DrawIndexed(
									b.mesh->indexCount,
									b.mesh->indexType,
									0, 
									0,
									b.instanceCount,
									0);
							}
						});
				}
				else if (l.type == LightType::Point && pointShadows.size() < kMaxPointShadows)
				{
					// Point shadows use a cubemap R32_FLOAT distance map (color) + a temporary D32 depth buffer.
					const rhi::Extent2D cubeExtent{ 2048, 2048 };
					const auto cube = graph.CreateTexture(renderGraph::RGTextureDesc{
						.extent = cubeExtent,
						.format = rhi::Format::R32_FLOAT,
						.usage = renderGraph::ResourceUsage::RenderTarget,
						.type = renderGraph::TextureType::Cube,
						.debugName = "PointShadowCube"
					});

					const auto depthTmp = graph.CreateTexture(renderGraph::RGTextureDesc{
						.extent = cubeExtent,
						.format = rhi::Format::D32_FLOAT,
						.usage = renderGraph::ResourceUsage::DepthStencil,
						.debugName = "PointShadowDepthTmp"
					});

					PointShadowRec rec{};
					rec.cube = cube;
					rec.depthTmp = depthTmp;
					rec.pos = l.position;
					rec.range = std::max(1.0f, l.range);
					rec.lightIndex = li;
					pointShadows.push_back(rec);

					auto FaceView = [](const mathUtils::Vec3& pos, int face) -> mathUtils::Mat4
					{
						// +X, -X, +Y, -Y, +Z, -Z
						static const mathUtils::Vec3 dirs[6] = {
							{ 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }
						};

						static const mathUtils::Vec3 ups[6] = {
							{ 0, 1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }, { 0, 1, 0 }, { 0, 1, 0 }
						};
						return mathUtils::LookAtRH(pos, pos + dirs[face], ups[face]);
					};

					const mathUtils::Mat4 proj90 = mathUtils::PerspectiveRH_ZO(mathUtils::ToRadians(90.0f), 1.0f, 0.1f, rec.range);

					for (int face = 0; face < 6; ++face)
					{
						const mathUtils::Mat4 vp = proj90 * FaceView(rec.pos, face);

						rhi::ClearDesc clear{};
						clear.clearColor = true;
						clear.clearDepth = true;

						clear.color = { 1.0f, 1.0f, 1.0f, 1.0f }; // far
						//float id = (float)face / 5.0f;           // 0..1
						//clear.color = { id, 0.0f, 0.0f, 1.0f };

						clear.depth = 1.0f;

						renderGraph::PassAttachments att{};
						att.useSwapChainBackbuffer = false;
						att.color = cube;
						att.colorCubeFace = static_cast<std::uint32_t>(face);
						att.depth = depthTmp;
						att.clearDesc = clear;

						const std::string passName =
							"PointShadowPass_" + std::to_string(static_cast<int>(pointShadows.size() - 1)) +
							"_F" + std::to_string(face);

						struct alignas(16) PointShadowConstants
						{
							std::array<float, 16> uFaceViewProj{};
							std::array<float, 4>  uLightPosRange{}; // xyz + range
							std::array<float, 4>  uMisc{};          // unused (bias is texel-based in main shader)
						};

						PointShadowConstants c{};
						const mathUtils::Mat4 vpT = mathUtils::Transpose(vp);
						std::memcpy(c.uFaceViewProj.data(), mathUtils::ValuePtr(vpT), sizeof(float) * 16);
						c.uLightPosRange = { rec.pos.x, rec.pos.y, rec.pos.z, rec.range };
						c.uMisc = { 0, 0, 0, 0 };


						graph.AddPass(passName, std::move(att),
							[this, c, shadowBatches, instStride](renderGraph::PassContext& ctx) mutable
							{
								ctx.commandList.SetViewport(0, 0,
									static_cast<int>(ctx.passExtent.width),
									static_cast<int>(ctx.passExtent.height));

								ctx.commandList.SetState(pointShadowState_);
								ctx.commandList.BindPipeline(psoPointShadow_);

								ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &c, 1 }));

								for (const ShadowBatch& shadowBatch : shadowBatches)
								{
									if (!shadowBatch.mesh || shadowBatch.instanceCount == 0) continue;

									ctx.commandList.BindInputLayout(shadowBatch.mesh->layoutInstanced);

									ctx.commandList.BindVertexBuffer(0, shadowBatch.mesh->vertexBuffer, shadowBatch.mesh->vertexStrideBytes, 0);
									ctx.commandList.BindVertexBuffer(1, instanceBuffer_, instStride, shadowBatch.instanceOffset * instStride);

									ctx.commandList.BindIndexBuffer(shadowBatch.mesh->indexBuffer, shadowBatch.mesh->indexType, 0);

									ctx.commandList.DrawIndexed(
										shadowBatch.mesh->indexCount,
										shadowBatch.mesh->indexType,
										0, 
										0,
										shadowBatch.instanceCount,
										0);
								}
							});
					}
				}
			}

			// Upload shadow metadata (t11).
			{
				ShadowDataSB sd{};

				for (std::size_t i = 0; i < spotShadows.size(); ++i)
				{
					const auto& s = spotShadows[i];

					const mathUtils::Mat4 vp = s.viewProj;

					sd.spotVPRows[i * 4 + 0] = vp[0];
					sd.spotVPRows[i * 4 + 1] = vp[1];
					sd.spotVPRows[i * 4 + 2] = vp[2];
					sd.spotVPRows[i * 4 + 3] = vp[3];

					sd.spotInfo[i] = mathUtils::Vec4(AsFloatBits(s.lightIndex), 0, settings_.spotShadowBaseBiasTexels, 0);
				}

				for (std::size_t i = 0; i < pointShadows.size(); ++i)
				{
					const auto& p = pointShadows[i];
					sd.pointPosRange[i] = mathUtils::Vec4(p.pos, p.range);
					sd.pointInfo[i] = mathUtils::Vec4(AsFloatBits(p.lightIndex), 0, settings_.pointShadowBaseBiasTexels, 0);
				}

				device_.UpdateBuffer(shadowDataBuffer_, std::as_bytes(std::span{ &sd, 1 }));
			}

			// ---------------- Main pass (swapchain) ----------------
			rhi::ClearDesc clearDesc{};
			clearDesc.clearColor = true;
			clearDesc.clearDepth = true;
			clearDesc.color = { 0.1f, 0.1f, 0.1f, 1.0f };

			graph.AddSwapChainPass("MainPass", clearDesc,
				[this, &scene, shadowRG, dirLightViewProj, lightCount, spotShadows, pointShadows, mainBatches, instStride](renderGraph::PassContext& ctx)
				{
					const auto extent = ctx.passExtent;

					ctx.commandList.SetViewport(0, 0,
						static_cast<int>(extent.width),
						static_cast<int>(extent.height));

					ctx.commandList.SetState(state_);

					// Bind directional shadow map at slot 1 (t1)
					{
						const auto shadowTex = ctx.resources.GetTexture(shadowRG);
						ctx.commandList.BindTexture2D(1, shadowTex);
					}

					// Bind Spot shadow maps at t3..t6 and Point shadow cubemaps at t7..t10.
					for (std::size_t i = 0; i < spotShadows.size(); ++i)
					{
						const auto tex = ctx.resources.GetTexture(spotShadows[i].tex);
						ctx.commandList.BindTexture2D(3 + static_cast<std::uint32_t>(i), tex);
					}
					for (std::size_t i = 0; i < pointShadows.size(); ++i)
					{
						const auto tex = ctx.resources.GetTexture(pointShadows[i].cube);
						ctx.commandList.BindTextureCube(7 + static_cast<std::uint32_t>(i), tex);
					}

					// Bind shadow metadata SB at t11
					ctx.commandList.BindStructuredBufferSRV(11, shadowDataBuffer_);

					const float aspect = extent.height
						? (static_cast<float>(extent.width) / static_cast<float>(extent.height))
						: 1.0f;

					const mathUtils::Mat4 proj = mathUtils::PerspectiveRH_ZO(mathUtils::ToRadians(scene.camera.fovYDeg), aspect, scene.camera.nearZ, scene.camera.farZ);
					const mathUtils::Mat4 view = mathUtils::LookAt(scene.camera.position, scene.camera.target, scene.camera.up);
					const mathUtils::Vec3 camPosLocal = scene.camera.position;

					const mathUtils::Mat4 viewProj = proj * view;

					// Bind lights (t2 StructuredBuffer SRV)
					ctx.commandList.BindStructuredBufferSRV(2, lightsBuffer_);

					constexpr std::uint32_t kFlagUseTex = 1u << 0;
					constexpr std::uint32_t kFlagUseShadow = 1u << 1;

					for (const Batch& b : mainBatches)
					{
						if (!b.mesh || b.instanceCount == 0)
						{
							continue;
						}

						MaterialPerm perm = MaterialPerm::UseShadow;
						if (b.materialHandle.id != 0)
						{
							perm = EffectivePerm(scene.GetMaterial(b.materialHandle));
						}
						else
						{
							// Fallback: infer only from params.
							if (b.material.albedoDescIndex != 0)
							{
								perm = perm | MaterialPerm::UseTex;
							}
						}

						const bool useTex = HasFlag(perm, MaterialPerm::UseTex);
						const bool useShadow = HasFlag(perm, MaterialPerm::UseShadow);

						ctx.commandList.BindPipeline(MainPipelineFor(perm));
						ctx.commandList.BindTextureDesc(0, b.material.albedoDescIndex);

						std::uint32_t flags = 0;
						if (useTex)
						{
							flags |= kFlagUseTex;
						}
						if (useShadow)
						{
							flags |= kFlagUseShadow;
						}

						PerBatchConstants constants{};
						const mathUtils::Mat4 viewProjT = mathUtils::Transpose(viewProj);
						const mathUtils::Mat4 dirVP_T = mathUtils::Transpose(dirLightViewProj);

						std::memcpy(constants.uViewProj.data(), mathUtils::ValuePtr(viewProjT), sizeof(float) * 16);
						std::memcpy(constants.uLightViewProj.data(), mathUtils::ValuePtr(dirVP_T), sizeof(float) * 16);

						constants.uCameraAmbient = { camPosLocal.x, camPosLocal.y, camPosLocal.z, 0.22f };
						constants.uBaseColor = { b.material.baseColor.x, b.material.baseColor.y, b.material.baseColor.z, b.material.baseColor.w };

						const float materialBiasTexels = b.material.shadowBias;
						constants.uMaterialFlags = { b.material.shininess, b.material.specStrength, materialBiasTexels, AsFloatBits(flags) };

						constants.uCounts = {
							static_cast<float>(lightCount),
							static_cast<float>(spotShadows.size()),
							static_cast<float>(pointShadows.size()),
							0.0f
						};

						constants.uShadowBias = {
							settings_.dirShadowBaseBiasTexels,
							settings_.spotShadowBaseBiasTexels,
							settings_.pointShadowBaseBiasTexels,
							settings_.shadowSlopeScaleTexels
						};

						// IA (instanced)
						ctx.commandList.BindInputLayout(b.mesh->layoutInstanced);
						ctx.commandList.BindVertexBuffer(0, b.mesh->vertexBuffer, b.mesh->vertexStrideBytes, 0);
						ctx.commandList.BindVertexBuffer(1, instanceBuffer_, instStride, b.instanceOffset * instStride);
						ctx.commandList.BindIndexBuffer(b.mesh->indexBuffer, b.mesh->indexType, 0);

						ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &constants, 1 }));
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
			if (lightsBuffer_)
			{
				device_.DestroyBuffer(lightsBuffer_);
			}
			if (shadowDataBuffer_)
			{
				device_.DestroyBuffer(shadowDataBuffer_);
			}
			psoCache_.ClearCache();
			shaderLibrary_.ClearCache();
		}

	private:
		rhi::PipelineHandle MainPipelineFor(MaterialPerm perm) const noexcept
		{
			const bool useTex = HasFlag(perm, MaterialPerm::UseTex);
			const bool useShadow = HasFlag(perm, MaterialPerm::UseShadow);
			const std::uint32_t idx = (useTex ? 1u : 0u) | (useShadow ? 2u : 0u);
			return psoMain_[idx];
		}

		static float AsFloatBits(std::uint32_t u) noexcept
		{
			float f{};
			std::memcpy(&f, &u, sizeof(u));
			return f;
		}

		std::uint32_t UploadLights(const Scene& scene, const mathUtils::Vec3& camPos)
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

				const float cosOuter = std::cos(mathUtils::ToRadians(l.outerHalfAngleDeg));
				const float cosInner = std::cos(mathUtils::ToRadians(l.innerHalfAngleDeg));

				out.p3 = { cosInner, cosOuter, l.attLinear, l.attQuadratic };
				gpu.push_back(out);
			}

			// Small default rig if the scene didn't provide any lights
			if (gpu.empty())
			{
				GPULight dir{};
				const mathUtils::Vec3 dirFromLight = mathUtils::Normalize(mathUtils::Vec3(-0.4f, -1.0f, -0.3f));
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
				const mathUtils::Vec3 spotPos = camPos;
				const mathUtils::Vec3 spotDir = mathUtils::Normalize(mathUtils::Vec3(0, 0, 0) - camPos);
				spot.p0 = { spotPos.x, spotPos.y, spotPos.z, static_cast<float>(static_cast<std::uint32_t>(LightType::Spot)) };
				spot.p1 = { spotDir.x, spotDir.y, spotDir.z, 3.0f };
				spot.p2 = { 0.8f, 0.9f, 1.0f, 30.0f };
				spot.p3 = { std::cos(mathUtils::ToRadians(12.0f)), std::cos(mathUtils::ToRadians(20.0f)), 0.09f, 0.032f };
				gpu.push_back(spot);
			}

			device_.UpdateBuffer(lightsBuffer_, std::as_bytes(std::span{ gpu }));
			return static_cast<std::uint32_t>(gpu.size());
		}

		void CreateResources()
		{
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

			// Main pipeline permutations (UseTex / UseShadow)
			{
				auto MakeDefines = [](bool useTex, bool useShadow) -> std::vector<std::string>
				{
					std::vector<std::string> d;
					if (useTex)
					{
						d.push_back("USE_TEX=1");
					}
					if (useShadow)
					{
						d.push_back("USE_SHADOW=1");
					}
					return d;
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
					if (useTex) psoName += "_Tex";
					if (useShadow) psoName += "_Shadow";

					psoMain_[idx] = psoCache_.GetOrCreate(psoName, vs, ps);
				}

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

			// Point shadow pipeline (R32_FLOAT distance cubemap)
			if (device_.GetBackend() == rhi::Backend::DirectX12)
			{
				const auto vsPoint = shaderLibrary_.GetOrCreateShader(ShaderKey{
					.stage = rhi::ShaderStage::Vertex,
					.name = "VS_ShadowPoint",
					.filePath = pointShadowPath.string(),
					.defines = {}
				});
				const auto psPoint = shaderLibrary_.GetOrCreateShader(ShaderKey{
					.stage = rhi::ShaderStage::Pixel,
					.name = "PS_ShadowPoint",
					.filePath = pointShadowPath.string(),
					.defines = {}
				});
				psoPointShadow_ = psoCache_.GetOrCreate("PSO_PointShadow", vsPoint, psPoint);

				pointShadowState_.depth.testEnable = true;
				pointShadowState_.depth.writeEnable = true;
				pointShadowState_.depth.depthCompareOp = rhi::CompareOp::LessEqual;

				pointShadowState_.rasterizer.cullMode = rhi::CullMode::None;
				pointShadowState_.rasterizer.frontFace = rhi::FrontFace::CounterClockwise;

				pointShadowState_.blend.enable = false;
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


				// Shadow metadata structured buffer (t11) — holds spot VP rows + indices/bias, and point pos/range + indices/bias.
				{
					rhi::BufferDesc sd{};
					sd.bindFlag = rhi::BufferBindFlag::StructuredBuffer;
					sd.usageFlag = rhi::BufferUsageFlag::Dynamic;
					sd.sizeInBytes = sizeof(ShadowDataSB);
					sd.structuredStrideBytes = static_cast<std::uint32_t>(sizeof(ShadowDataSB));
					sd.debugName = "ShadowDataSB";
					shadowDataBuffer_ = device_.CreateBuffer(sd);
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
			}
		}

	private:
		static constexpr std::uint32_t kMaxLights = 64;
		static constexpr std::uint32_t kDefaultInstanceBufferSizeBytes = 8u * 1024u * 1024u; // 8 MB (combined shadow+main instances)

		rhi::IRHIDevice& device_;
		RendererSettings settings_{};

		ShaderLibrary shaderLibrary_;
		PSOCache psoCache_;

		// Main pass
		std::array<rhi::PipelineHandle, 4> psoMain_{}; // idx: (UseTex?1:0)|(UseShadow?2:0)
		rhi::GraphicsState state_{};

		rhi::BufferHandle instanceBuffer_{};
		std::uint32_t instanceBufferSizeBytes_{ kDefaultInstanceBufferSizeBytes };


		// Shadow pass
		rhi::PipelineHandle psoShadow_{};
		rhi::GraphicsState shadowState_{};

		rhi::BufferHandle lightsBuffer_{};
		rhi::BufferHandle shadowDataBuffer_{};

		// Point shadow pass (R32_FLOAT distance cubemap)
		rhi::PipelineHandle psoPointShadow_{};
		rhi::GraphicsState pointShadowState_{};
	};
} // namespace rendern