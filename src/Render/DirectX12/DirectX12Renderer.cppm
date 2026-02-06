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
#include <optional>
#include <stdexcept>
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
		static std::size_t HashU32(std::uint32_t value) noexcept { return std::hash<std::uint32_t>{}(value); }
		static std::size_t HashPtr(const void* ptr) noexcept { return std::hash<const void*>{}(ptr); }

		static std::uint32_t FloatBits(float value) noexcept
		{
			std::uint32_t bits{};
			std::memcpy(&bits, &value, sizeof(bits));
			return bits;
		}

		static void HashCombine(std::size_t& seed, std::size_t value) noexcept
		{
			seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
		}

		std::size_t operator()(const BatchKey& key) const noexcept
		{
			std::size_t seed = HashPtr(key.mesh);
			HashCombine(seed, HashU32(static_cast<std::uint32_t>(key.albedoDescIndex)));
			HashCombine(seed, HashU32(FloatBits(key.baseColor.x)));
			HashCombine(seed, HashU32(FloatBits(key.baseColor.y)));
			HashCombine(seed, HashU32(FloatBits(key.baseColor.z)));
			HashCombine(seed, HashU32(FloatBits(key.baseColor.w)));
			HashCombine(seed, HashU32(FloatBits(key.shininess)));
			HashCombine(seed, HashU32(FloatBits(key.specStrength)));
			HashCombine(seed, HashU32(FloatBits(key.shadowBias)));
			return seed;
		}
	};

	struct BatchKeyEq
	{
		bool operator()(const BatchKey& lhs, const BatchKey& rhs) const noexcept
		{
			return lhs.mesh == rhs.mesh &&
				lhs.albedoDescIndex == rhs.albedoDescIndex &&
				lhs.baseColor == rhs.baseColor &&
				lhs.shininess == rhs.shininess &&
				lhs.specStrength == rhs.specStrength &&
				lhs.shadowBias == rhs.shadowBias;
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
		std::uint32_t lightIndex{ 0 };
	};

	struct PointShadowRec
	{
		renderGraph::RGTextureHandle cube{};
		renderGraph::RGTextureHandle depthTmp{};
		mathUtils::Vec3 pos{};
		float range{ 10.0f };;
		std::uint32_t lightIndex{ 0 };
	};

	struct alignas(16) ShadowConstants
	{
		std::array<float, 16> uMVP{}; // lightProj * lightView * model
	};

	struct ShadowBatch
	{
		const rendern::MeshRHI* mesh{};
		std::uint32_t instanceOffset{ 0 }; // in combinedInstances[]
		std::uint32_t instanceCount{ 0 };
	};

	struct TransparentDraw
	{
		const rendern::MeshRHI* mesh{};
		MaterialParams material{};
		MaterialHandle materialHandle{};
		std::uint32_t instanceOffset{ 0 }; // absolute offset in combined instance buffer
		float dist2{ 0.0f };               // for sorting (bigger first)
	};

	struct TransparentTemp
	{
		const rendern::MeshRHI* mesh{};
		MaterialParams material{};
		MaterialHandle materialHandle{};
		std::uint32_t localInstanceOffset{};
		float dist2{};
	};

	struct alignas(16) SkyboxConstants
	{
		std::array<float, 16> uViewProj{};
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

		void SetSettings(const RendererSettings& settings)
		{
			settings_ = settings;
		}

		void RenderFrame(rhi::IRHISwapChain& swapChain, const Scene& scene, const void* imguiDrawData)
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
			for (const auto& light : scene.lights)
			{
				if (light.type == LightType::Directional)
				{
					lightDir = mathUtils::Normalize(light.direction);
					break;
				}
			}

			// --------------------------------
			// Fit directional shadow ortho projection to a camera frustum slice in light-space.
			// This prevents hard "shadow coverage clipping" when the camera rotates.
			const rhi::SwapChainDesc scDesc = swapChain.GetDesc();
			const float aspect = (scDesc.extent.height > 0)
				? (static_cast<float>(scDesc.extent.width) / static_cast<float>(scDesc.extent.height))
				: 1.0f;

			// Limit how far we render directional shadows to keep resolution usable.
			const float shadowFar = std::min(scene.camera.farZ, 60.0f);
			const float shadowNear = std::max(scene.camera.nearZ, 0.05f);

			// Camera basis (orthonormal).
			const mathUtils::Vec3 camF = mathUtils::Normalize(scene.camera.target - scene.camera.position);
			mathUtils::Vec3 camR = mathUtils::Cross(camF, scene.camera.up);
			camR = mathUtils::Normalize(camR);
			const mathUtils::Vec3 camU = mathUtils::Cross(camR, camF);

			const float fovY = mathUtils::DegToRad(scene.camera.fovYDeg);
			const float tanHalf = std::tan(fovY * 0.5f);

			auto MakeFrustumCorner = [&](float dist, float sx, float sy) -> mathUtils::Vec3
				{
					// sx,sy are in {-1,+1} (left/right, bottom/top).
					const float halfH = dist * tanHalf;
					const float halfW = halfH * aspect;
					const mathUtils::Vec3 planeCenter = scene.camera.position + camF * dist;
					return planeCenter + camU * (sy * halfH) + camR * (sx * halfW);
				};

			std::array<mathUtils::Vec3, 8> frustumCorners{};
			// Near plane
			frustumCorners[0] = MakeFrustumCorner(shadowNear, -1.0f, -1.0f);
			frustumCorners[1] = MakeFrustumCorner(shadowNear, 1.0f, -1.0f);
			frustumCorners[2] = MakeFrustumCorner(shadowNear, 1.0f, 1.0f);
			frustumCorners[3] = MakeFrustumCorner(shadowNear, -1.0f, 1.0f);
			// Far plane (shadow distance)
			frustumCorners[4] = MakeFrustumCorner(shadowFar, -1.0f, -1.0f);
			frustumCorners[5] = MakeFrustumCorner(shadowFar, 1.0f, -1.0f);
			frustumCorners[6] = MakeFrustumCorner(shadowFar, 1.0f, 1.0f);
			frustumCorners[7] = MakeFrustumCorner(shadowFar, -1.0f, 1.0f);

			// Frustum center + radius (for stable light placement).
			mathUtils::Vec3 center{ 0.0f, 0.0f, 0.0f };
			for (const auto& corner : frustumCorners)
			{
				center = center + corner;
			}
			center = center * (1.0f / 8.0f);

			float radius = 0.0f;
			for (const auto& corner : frustumCorners)
			{
				radius = std::max(radius, mathUtils::Length(corner - center));
			}

			// Stable "up" for light view.
			const mathUtils::Vec3 worldUp(0.0f, 1.0f, 0.0f);
			const mathUtils::Vec3 lightUp = (std::abs(mathUtils::Dot(lightDir, worldUp)) > 0.99f)
				? mathUtils::Vec3(0.0f, 0.0f, 1.0f)
				: worldUp;

			// Place the light far enough so all corners are in front of it.
			const float lightDist = radius + 100.0f;
			const mathUtils::Vec3 lightPos = center - lightDir * lightDist;
			const mathUtils::Mat4 lightView = mathUtils::LookAt(lightPos, center, lightUp);

			// Compute light-space AABB of the camera frustum slice.
			float minX = 1e30f, minY = 1e30f, minZ = 1e30f;
			float maxX = -1e30f, maxY = -1e30f, maxZ = -1e30f;

			for (const auto& corner : frustumCorners)
			{
				const mathUtils::Vec4 ls4 = lightView * mathUtils::Vec4(corner, 1.0f);
				minX = std::min(minX, ls4.x); maxX = std::max(maxX, ls4.x);
				minY = std::min(minY, ls4.y); maxY = std::max(maxY, ls4.y);
				minZ = std::min(minZ, ls4.z); maxZ = std::max(maxZ, ls4.z);
			}

			// Padding to avoid hard coverage clipping.
			const float extX = maxX - minX;
			const float extY = maxY - minY;
			const float extZ = maxZ - minZ;

			const float padXY = 0.10f * std::max(extX, extY) + 2.0f;
			const float padZ = 0.20f * extZ + 10.0f;

			minX -= padXY; maxX += padXY;
			minY -= padXY; maxY += padXY;
			minZ -= padZ;  maxZ += padZ;

			// Extra depth margin for casters outside the camera frustum (important for directional lights).
			constexpr float casterMargin = 80.0f;
			minZ -= casterMargin;

			// Snap the ortho window to shadow texels (reduces shimmering / "special angle" popping).
			const float widthLS = maxX - minX;
			const float heightLS = maxY - minY;

			const float wuPerTexelX = widthLS / static_cast<float>(shadowExtent.width);
			const float wuPerTexelY = heightLS / static_cast<float>(shadowExtent.height);

			float cx = 0.5f * (minX + maxX);
			float cy = 0.5f * (minY + maxY);

			cx = std::floor(cx / wuPerTexelX) * wuPerTexelX;
			cy = std::floor(cy / wuPerTexelY) * wuPerTexelY;

			minX = cx - widthLS * 0.5f;  maxX = cx + widthLS * 0.5f;
			minY = cy - heightLS * 0.5f; maxY = cy + heightLS * 0.5f;

			// OrthoRH_ZO expects positive zNear/zFar distances where view-space z is negative in front of the camera.
			const float zNear = std::max(0.1f, -maxZ);
			const float zFar = std::max(zNear + 1.0f, -minZ);

			const mathUtils::Mat4 lightProj = mathUtils::OrthoRH_ZO(minX, maxX, minY, maxY, zNear, zFar);
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

				// IMPORTANT: exclude alpha-blended objects from shadow casting
				MaterialParams params{};
				MaterialPerm perm = MaterialPerm::UseShadow;
				if (item.material.id != 0)
				{
					const auto& mat = scene.GetMaterial(item.material);
					params = mat.params;
					perm = EffectivePerm(mat);
				}
				else
				{
					params.baseColor = { 1,1,1,1 };
					params.shininess = 32.0f;
					params.specStrength = 0.2f;
					params.shadowBias = 0.0f;
					params.albedoDescIndex = 0;
					perm = MaterialPerm::UseShadow;
				}

				const bool isTransparent = HasFlag(perm, MaterialPerm::Transparent) || (params.baseColor.w < 0.999f);
				if (isTransparent)
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
					if (!mesh || vec.empty())
					{
						continue;
					}

					ShadowBatch shadowBatch{};
					shadowBatch.mesh = mesh;
					shadowBatch.instanceOffset = static_cast<std::uint32_t>(shadowInstances.size());
					shadowBatch.instanceCount = static_cast<std::uint32_t>(vec.size());

					shadowInstances.insert(shadowInstances.end(), vec.begin(), vec.end());
					shadowBatches.push_back(shadowBatch);
				}
			}

			// ---- Main packing: opaque (batched) + transparent (sorted per-item) ----
			std::unordered_map<BatchKey, BatchTemp, BatchKeyHash, BatchKeyEq> mainTmp;
			mainTmp.reserve(scene.drawItems.size());

			std::vector<InstanceData> transparentInstances;
			transparentInstances.reserve(scene.drawItems.size());

			std::vector<TransparentTemp> transparentTmp;
			transparentTmp.reserve(scene.drawItems.size());

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
				MaterialPerm perm = MaterialPerm::UseShadow;
				if (item.material.id != 0)
				{
					const auto& mat = scene.GetMaterial(item.material);
					params = mat.params;
					perm = EffectivePerm(mat);
				}
				else
				{
					params.baseColor = { 1,1,1,1 };
					params.shininess = 32.0f;
					params.specStrength = 0.2f;
					params.shadowBias = 0.0f;
					params.albedoDescIndex = 0;
					perm = MaterialPerm::UseShadow;
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

				const bool isTransparent = HasFlag(perm, MaterialPerm::Transparent) || (params.baseColor.w < 0.999f);
				if (isTransparent)
				{
					const mathUtils::Vec3 deltaToCamera = item.transform.position - camPos;
					const float dist2 = mathUtils::Dot(deltaToCamera, deltaToCamera);
					const std::uint32_t localOff = static_cast<std::uint32_t>(transparentInstances.size());
					transparentInstances.push_back(inst);
					transparentTmp.push_back(TransparentTemp{ mesh, params, item.material, localOff, dist2 });
					continue;
				}

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
				if (bt.inst.empty())
				{
					continue;
				}

				Batch batch{};
				batch.mesh = key.mesh;
				batch.materialHandle = bt.materialHandle;
				batch.material = bt.material;
				batch.instanceOffset = static_cast<std::uint32_t>(mainInstances.size());
				batch.instanceCount = static_cast<std::uint32_t>(bt.inst.size());

				mainInstances.insert(mainInstances.end(), bt.inst.begin(), bt.inst.end());
				mainBatches.push_back(batch);
			}

			// ---- Combine and upload once ----
			const std::uint32_t shadowBase = 0;
			const std::uint32_t mainBase = static_cast<std::uint32_t>(shadowInstances.size());
			const std::uint32_t transparentBase = static_cast<std::uint32_t>(shadowInstances.size() + mainInstances.size());

			for (auto& sbatch : shadowBatches)
			{
				sbatch.instanceOffset += shadowBase;
			}
			for (auto& mbatch : mainBatches)
			{
				mbatch.instanceOffset += mainBase;
			}

			std::vector<TransparentDraw> transparentDraws;
			transparentDraws.reserve(transparentTmp.size());
			for (const auto& transparentInst : transparentTmp)
			{
				TransparentDraw transparentDraw{};
				transparentDraw.mesh = transparentInst.mesh;
				transparentDraw.material = transparentInst.material;
				transparentDraw.materialHandle = transparentInst.materialHandle;
				transparentDraw.instanceOffset = transparentBase + transparentInst.localInstanceOffset;
				transparentDraw.dist2 = transparentInst.dist2;
				transparentDraws.push_back(transparentDraw);
			}

			std::sort(transparentDraws.begin(), transparentDraws.end(),
				[](const TransparentDraw& first, const TransparentDraw& second)
				{
					return first.dist2 > second.dist2; // far -> near
				});

			std::vector<InstanceData> combinedInstances;
			combinedInstances.reserve(shadowInstances.size() + mainInstances.size());
			combinedInstances.insert(combinedInstances.end(), shadowInstances.begin(), shadowInstances.end());
			combinedInstances.insert(combinedInstances.end(), mainInstances.begin(), mainInstances.end());
			combinedInstances.insert(combinedInstances.end(), transparentInstances.begin(), transparentInstances.end());

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

				ShadowPassConstants shadowPassConstants{};
				const mathUtils::Mat4 dirVP_T = mathUtils::Transpose(dirLightViewProj);
				std::memcpy(shadowPassConstants.uLightViewProj.data(), mathUtils::ValuePtr(dirVP_T), sizeof(float) * 16);

				graph.AddPass("ShadowPass", std::move(att),
					[this, shadowPassConstants, shadowBatches, instStride](renderGraph::PassContext& ctx) mutable
					{
						ctx.commandList.SetViewport(0, 0,
							static_cast<int>(ctx.passExtent.width),
							static_cast<int>(ctx.passExtent.height));

						ctx.commandList.SetState(shadowState_);
						ctx.commandList.BindPipeline(psoShadow_);

						ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &shadowPassConstants, 1 }));

						this->DrawInstancedShadowBatches(ctx.commandList, shadowBatches, instStride);

					});
			}

			// Collect up to kMaxSpotShadows / kMaxPointShadows from scene.lights (index aligns with UploadLights()).
			for (std::uint32_t lightIndex = 0; lightIndex < static_cast<std::uint32_t>(scene.lights.size()); ++lightIndex)
			{
				if (lightIndex >= kMaxLights)
				{
					break;
				}

				const auto& light = scene.lights[lightIndex];

				if (light.type == LightType::Spot && spotShadows.size() < kMaxSpotShadows)
				{
					const rhi::Extent2D ext{ 1024, 1024 };
					const auto rg = graph.CreateTexture(renderGraph::RGTextureDesc{
						.extent = ext,
						.format = rhi::Format::D32_FLOAT,
						.usage = renderGraph::ResourceUsage::DepthStencil,
						.debugName = "SpotShadowMap"
						});

					mathUtils::Vec3 lightDirLocal = mathUtils::Normalize(light.direction);
					mathUtils::Vec3 upVector = (std::abs(mathUtils::Dot(lightDirLocal, mathUtils::Vec3(0, 1, 0))) > 0.99f)
						? mathUtils::Vec3(0, 0, 1)
						: mathUtils::Vec3(0, 1, 0);

					mathUtils::Mat4 lightView = mathUtils::LookAt(light.position, light.position + lightDirLocal, upVector);

					const float outerHalf = std::max(1.0f, light.outerHalfAngleDeg);
					const float farZ = std::max(1.0f, light.range);
					const float nearZ = std::max(0.5f, farZ * 0.02f);
					const mathUtils::Mat4 lightProj = mathUtils::PerspectiveRH_ZO(mathUtils::DegToRad(outerHalf * 2.0f), 1.0f, nearZ, farZ);
					const mathUtils::Mat4 lightViewProj = lightProj * lightView;

					SpotShadowRec rec{};
					rec.tex = rg;
					rec.viewProj = lightViewProj;
					rec.lightIndex = lightIndex;
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
					SpotPassConstants spotPassConstants{};
					const mathUtils::Mat4 lightViewProjTranspose = mathUtils::Transpose(lightViewProj);
					std::memcpy(spotPassConstants.uLightViewProj.data(), mathUtils::ValuePtr(lightViewProjTranspose), sizeof(float) * 16);

					graph.AddPass(passName, std::move(att),
						[this, spotPassConstants, shadowBatches, instStride](renderGraph::PassContext& ctx) mutable
						{
							ctx.commandList.SetViewport(0, 0,
								static_cast<int>(ctx.passExtent.width),
								static_cast<int>(ctx.passExtent.height));

							ctx.commandList.SetState(shadowState_);
							ctx.commandList.BindPipeline(psoShadow_);

							ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &spotPassConstants, 1 }));

							this->DrawInstancedShadowBatches(ctx.commandList, shadowBatches, instStride);

						});
				}
				else if (light.type == LightType::Point && pointShadows.size() < kMaxPointShadows)
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
					rec.pos = light.position;
					rec.range = std::max(1.0f, light.range);
					rec.lightIndex = lightIndex;
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

					const mathUtils::Mat4 proj90 = mathUtils::PerspectiveRH_ZO(mathUtils::DegToRad(90.0f), 1.0f, 0.1f, rec.range);

					for (int face = 0; face < 6; ++face)
					{
						const mathUtils::Mat4 faceViewProj = proj90 * FaceView(rec.pos, face);

						rhi::ClearDesc clear{};
						clear.clearColor = true;
						clear.clearDepth = true;

						clear.color = { 1.0f, 1.0f, 1.0f, 1.0f }; // far
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

						PointShadowConstants pointShadowConstants{};
						const mathUtils::Mat4 faceViewProjTranspose = mathUtils::Transpose(faceViewProj);
						std::memcpy(pointShadowConstants.uFaceViewProj.data(), mathUtils::ValuePtr(faceViewProjTranspose), sizeof(float) * 16);
						pointShadowConstants.uLightPosRange = { rec.pos.x, rec.pos.y, rec.pos.z, rec.range };
						pointShadowConstants.uMisc = { 0, 0, 0, 0 };


						graph.AddPass(passName, std::move(att),
							[this, pointShadowConstants, shadowBatches, instStride](renderGraph::PassContext& ctx) mutable
							{
								ctx.commandList.SetViewport(0, 0,
									static_cast<int>(ctx.passExtent.width),
									static_cast<int>(ctx.passExtent.height));

								ctx.commandList.SetState(pointShadowState_);
								ctx.commandList.BindPipeline(psoPointShadow_);

								ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &pointShadowConstants, 1 }));

								this->DrawInstancedShadowBatches(ctx.commandList, shadowBatches, instStride);

							});
					}
				}
			}

			// Upload shadow metadata (t11).
			{
				ShadowDataSB sd{};

				for (std::size_t spotShadowIndex = 0; spotShadowIndex < spotShadows.size(); ++spotShadowIndex)
				{
					const auto& spotShadow = spotShadows[spotShadowIndex];

					const mathUtils::Mat4 vp = spotShadow.viewProj;

					sd.spotVPRows[spotShadowIndex * 4 + 0] = vp[0];
					sd.spotVPRows[spotShadowIndex * 4 + 1] = vp[1];
					sd.spotVPRows[spotShadowIndex * 4 + 2] = vp[2];
					sd.spotVPRows[spotShadowIndex * 4 + 3] = vp[3];

					sd.spotInfo[spotShadowIndex] = mathUtils::Vec4(AsFloatBits(spotShadow.lightIndex), 0, settings_.spotShadowBaseBiasTexels, 0);
				}

				for (std::size_t pointShadowIndex = 0; pointShadowIndex < pointShadows.size(); ++pointShadowIndex)
				{
					const auto& pointShadow = pointShadows[pointShadowIndex];
					sd.pointPosRange[pointShadowIndex] = mathUtils::Vec4(pointShadow.pos, pointShadow.range);
					sd.pointInfo[pointShadowIndex] = mathUtils::Vec4(AsFloatBits(pointShadow.lightIndex), 0, settings_.pointShadowBaseBiasTexels, 0);
				}

				device_.UpdateBuffer(shadowDataBuffer_, std::as_bytes(std::span{ &sd, 1 }));
			}

			// ---------------- Main pass (swapchain) ----------------
			rhi::ClearDesc clearDesc{};
			clearDesc.clearColor = true;
			clearDesc.clearDepth = true;
			clearDesc.color = { 0.1f, 0.1f, 0.1f, 1.0f };

			graph.AddSwapChainPass("MainPass", clearDesc,
				[this, &scene, shadowRG, dirLightViewProj, lightCount, spotShadows, pointShadows, mainBatches, instStride, transparentDraws, imguiDrawData](renderGraph::PassContext& ctx)
				{
					const auto extent = ctx.passExtent;

					ctx.commandList.SetViewport(0, 0,
						static_cast<int>(extent.width),
						static_cast<int>(extent.height));

					ctx.commandList.SetState(state_);

					const float aspect = extent.height
						? (static_cast<float>(extent.width) / static_cast<float>(extent.height))
						: 1.0f;

					const mathUtils::Mat4 proj = mathUtils::PerspectiveRH_ZO(mathUtils::DegToRad(scene.camera.fovYDeg), aspect, scene.camera.nearZ, scene.camera.farZ);
					const mathUtils::Mat4 view = mathUtils::LookAt(scene.camera.position, scene.camera.target, scene.camera.up);
					const mathUtils::Vec3 camPosLocal = scene.camera.position;

					const mathUtils::Mat4 viewProj = proj * view;


					// --- Skybox draw ---
					{
						if (scene.skyboxDescIndex != 0)
						{
							mathUtils::Mat4 viewNoTranslation = view;
							viewNoTranslation[3] = mathUtils::Vec4(0, 0, 0, 1);

							const mathUtils::Mat4 viewProjSkybox = proj * viewNoTranslation;
							const mathUtils::Mat4 viewProjSkyboxTranspose = mathUtils::Transpose(viewProjSkybox);

							SkyboxConstants skyboxConstants{};
							std::memcpy(skyboxConstants.uViewProj.data(), mathUtils::ValuePtr(viewProjSkyboxTranspose), sizeof(float) * 16);

							ctx.commandList.SetState(skyboxState_);
							ctx.commandList.BindPipeline(psoSkybox_);
							ctx.commandList.BindTextureDesc(0, scene.skyboxDescIndex);

							ctx.commandList.BindInputLayout(skyboxMesh_.layout);
							ctx.commandList.BindVertexBuffer(0, skyboxMesh_.vertexBuffer, skyboxMesh_.vertexStrideBytes, 0);
							ctx.commandList.BindIndexBuffer(skyboxMesh_.indexBuffer, skyboxMesh_.indexType, 0);

							ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &skyboxConstants, 1 }));
							ctx.commandList.DrawIndexed(skyboxMesh_.indexCount, skyboxMesh_.indexType, 0, 0);

							ctx.commandList.SetState(state_);
						}
					}

					// Bind directional shadow map at slot 1 (t1)
					{
						const auto shadowTex = ctx.resources.GetTexture(shadowRG);
						ctx.commandList.BindTexture2D(1, shadowTex);
					}

					// Bind Spot shadow maps at t3..t6 and Point shadow cubemaps at t7..t10.
					for (std::size_t spotShadowIndex = 0; spotShadowIndex < spotShadows.size(); ++spotShadowIndex)
					{
						const auto tex = ctx.resources.GetTexture(spotShadows[spotShadowIndex].tex);
						ctx.commandList.BindTexture2D(3 + static_cast<std::uint32_t>(spotShadowIndex), tex);
					}
					for (std::size_t pointShadowIndex = 0; pointShadowIndex < pointShadows.size(); ++pointShadowIndex)
					{
						const auto tex = ctx.resources.GetTexture(pointShadows[pointShadowIndex].cube);
						ctx.commandList.BindTextureCube(7 + static_cast<std::uint32_t>(pointShadowIndex), tex);
					}

					// Bind shadow metadata SB at t11
					ctx.commandList.BindStructuredBufferSRV(11, shadowDataBuffer_);

					// Bind lights (t2 StructuredBuffer SRV)
					ctx.commandList.BindStructuredBufferSRV(2, lightsBuffer_);

					constexpr std::uint32_t kFlagUseTex = 1u << 0;
					constexpr std::uint32_t kFlagUseShadow = 1u << 1;

					for (const Batch& batch : mainBatches)
					{
						if (!batch.mesh || batch.instanceCount == 0)
						{
							continue;
						}

						MaterialPerm perm = MaterialPerm::UseShadow;
						if (batch.materialHandle.id != 0)
						{
							perm = EffectivePerm(scene.GetMaterial(batch.materialHandle));
						}
						else
						{
							// Fallback: infer only from params.
							if (batch.material.albedoDescIndex != 0)
							{
								perm = perm | MaterialPerm::UseTex;
							}
						}

						const bool useTex = HasFlag(perm, MaterialPerm::UseTex);
						const bool useShadow = HasFlag(perm, MaterialPerm::UseShadow);

						ctx.commandList.BindPipeline(MainPipelineFor(perm));
						ctx.commandList.BindTextureDesc(0, batch.material.albedoDescIndex);

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
						constants.uBaseColor = { batch.material.baseColor.x, batch.material.baseColor.y, batch.material.baseColor.z, batch.material.baseColor.w };

						const float materialBiasTexels = batch.material.shadowBias;
						constants.uMaterialFlags = { batch.material.shininess, batch.material.specStrength, materialBiasTexels, AsFloatBits(flags) };

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
						ctx.commandList.BindInputLayout(batch.mesh->layoutInstanced);
						ctx.commandList.BindVertexBuffer(0, batch.mesh->vertexBuffer, batch.mesh->vertexStrideBytes, 0);
						ctx.commandList.BindVertexBuffer(1, instanceBuffer_, instStride, batch.instanceOffset * instStride);
						ctx.commandList.BindIndexBuffer(batch.mesh->indexBuffer, batch.mesh->indexType, 0);

						ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &constants, 1 }));
						ctx.commandList.DrawIndexed(batch.mesh->indexCount, batch.mesh->indexType, 0, 0, batch.instanceCount, 0);
					}

					if (!transparentDraws.empty())
					{
						ctx.commandList.SetState(transparentState_);

						for (const TransparentDraw& batchTransparent : transparentDraws)
						{
							if (!batchTransparent.mesh)
							{
								continue;
							}

							MaterialPerm perm = MaterialPerm::UseShadow;
							if (batchTransparent.materialHandle.id != 0)
							{
								perm = EffectivePerm(scene.GetMaterial(batchTransparent.materialHandle));
							}
							else
							{
								if (batchTransparent.material.albedoDescIndex != 0)
								{
									perm = perm | MaterialPerm::UseTex;
								}
							}

							const bool useTex = HasFlag(perm, MaterialPerm::UseTex);
							const bool useShadow = HasFlag(perm, MaterialPerm::UseShadow);

							ctx.commandList.BindPipeline(MainPipelineFor(perm));
							ctx.commandList.BindTextureDesc(0, batchTransparent.material.albedoDescIndex);

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
							constants.uBaseColor = { batchTransparent.material.baseColor.x, batchTransparent.material.baseColor.y, batchTransparent.material.baseColor.z, batchTransparent.material.baseColor.w };

							const float materialBiasTexels = batchTransparent.material.shadowBias;
							constants.uMaterialFlags = { batchTransparent.material.shininess, batchTransparent.material.specStrength, materialBiasTexels, AsFloatBits(flags) };

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

							ctx.commandList.BindInputLayout(batchTransparent.mesh->layoutInstanced);
							ctx.commandList.BindVertexBuffer(0, batchTransparent.mesh->vertexBuffer, batchTransparent.mesh->vertexStrideBytes, 0);
							ctx.commandList.BindVertexBuffer(1, instanceBuffer_, instStride, batchTransparent.instanceOffset * instStride);
							ctx.commandList.BindIndexBuffer(batchTransparent.mesh->indexBuffer, batchTransparent.mesh->indexType, 0);

							ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &constants, 1 }));

							// IMPORTANT: transparent = one object per draw (instanceCount = 1)
							ctx.commandList.DrawIndexed(batchTransparent.mesh->indexCount, batchTransparent.mesh->indexType, 0, 0, 1, 0);
						}
					}


					// ImGui overlay (optional)
					if (imguiDrawData)
					{
						ctx.commandList.DX12ImGuiRender(imguiDrawData);
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
			DestroyMesh(device_, skyboxMesh_);
			psoCache_.ClearCache();
			shaderLibrary_.ClearCache();
		}

	private:

		void DrawInstancedShadowBatches(rhi::CommandList& commandList, std::span<const ShadowBatch> shadowBatches, std::uint32_t instanceStrideBytes) const
		{
			for (std::size_t batchIndex = 0; batchIndex < shadowBatches.size(); ++batchIndex)
			{
				const ShadowBatch& shadowBatch = shadowBatches[batchIndex];
				if (!shadowBatch.mesh || shadowBatch.instanceCount == 0)
				{
					continue;
				}

				commandList.BindInputLayout(shadowBatch.mesh->layoutInstanced);
				commandList.BindVertexBuffer(0, shadowBatch.mesh->vertexBuffer, shadowBatch.mesh->vertexStrideBytes, 0);
				commandList.BindVertexBuffer(1, instanceBuffer_, instanceStrideBytes, shadowBatch.instanceOffset * instanceStrideBytes);
				commandList.BindIndexBuffer(shadowBatch.mesh->indexBuffer, shadowBatch.mesh->indexType, 0);

				commandList.DrawIndexed(
					shadowBatch.mesh->indexCount,
					shadowBatch.mesh->indexType,
					0,
					0,
					shadowBatch.instanceCount,
					0);
			}
		}

		rhi::PipelineHandle MainPipelineFor(MaterialPerm perm) const noexcept
		{
			const bool useTex = HasFlag(perm, MaterialPerm::UseTex);
			const bool useShadow = HasFlag(perm, MaterialPerm::UseShadow);
			const std::uint32_t idx = (useTex ? 1u : 0u) | (useShadow ? 2u : 0u);
			return psoMain_[idx];
		}

		static float AsFloatBits(std::uint32_t packedBits) noexcept
		{
			float floatValue{};
			std::memcpy(&floatValue, &packedBits, sizeof(packedBits));
			return floatValue;
		}

		std::uint32_t UploadLights(const Scene& scene, const mathUtils::Vec3& camPos)
		{
			std::vector<GPULight> gpu;
			gpu.reserve(std::min<std::size_t>(scene.lights.size(), kMaxLights));

			for (const auto& light : scene.lights)
			{
				if (gpu.size() >= kMaxLights)
				{
					break;
				}

				GPULight gpuLight{};

				gpuLight.p0 = { light.position.x, light.position.y, light.position.z, static_cast<float>(static_cast<std::uint32_t>(light.type)) };
				gpuLight.p1 = { light.direction.x, light.direction.y, light.direction.z, light.intensity };
				gpuLight.p2 = { light.color.x, light.color.y, light.color.z, light.range };

				const float cosOuter = std::cos(mathUtils::DegToRad(light.outerHalfAngleDeg));
				const float cosInner = std::cos(mathUtils::DegToRad(light.innerHalfAngleDeg));

				gpuLight.p3 = { cosInner, cosOuter, light.attLinear, light.attQuadratic };
				gpu.push_back(gpuLight);
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
				spot.p3 = { std::cos(mathUtils::DegToRad(12.0f)), std::cos(mathUtils::DegToRad(20.0f)), 0.09f, 0.032f };
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

			// Main pipeline permutations (UseTex / UseShadow)
			{
				auto MakeDefines = [](bool useTex, bool useShadow) -> std::vector<std::string>
					{
						std::vector<std::string> defines;
						if (useTex)
						{
							defines.push_back("USE_TEX=1");
						}
						if (useShadow)
						{
							defines.push_back("USE_SHADOW=1");
						}
						return defines;
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
					if (useTex)
					{
						psoName += "_Tex";
					}
					if (useShadow)
					{
						psoName += "_Shadow";
					}

					psoMain_[idx] = psoCache_.GetOrCreate(psoName, vs, ps);
				}

				state_.depth.testEnable = true;
				state_.depth.writeEnable = true;
				state_.depth.depthCompareOp = rhi::CompareOp::LessEqual;

				state_.rasterizer.cullMode = rhi::CullMode::Back;
				state_.rasterizer.frontFace = rhi::FrontFace::CounterClockwise;

				state_.blend.enable = false;

				transparentState_ = state_;
				transparentState_.depth.writeEnable = false;
				transparentState_.blend.enable = true;
				transparentState_.rasterizer.cullMode = rhi::CullMode::None;
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
		rhi::GraphicsState transparentState_{};

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

		MeshRHI skyboxMesh_{};
		rhi::PipelineHandle psoSkybox_{};
		rhi::GraphicsState skyboxState_{};
	};
} // namespace rendern
