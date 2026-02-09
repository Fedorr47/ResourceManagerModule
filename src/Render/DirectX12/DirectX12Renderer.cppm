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
	rhi::TextureDescIndex normalDescIndex{};
	rhi::TextureDescIndex metalnessDescIndex{};
	rhi::TextureDescIndex roughnessDescIndex{};
	rhi::TextureDescIndex aoDescIndex{};
	rhi::TextureDescIndex emissiveDescIndex{};

	mathUtils::Vec4 baseColor{};
	float shadowBias{}; // texels

	// PBR scalars (used if corresponding texture isn't provided)
	float metallic{};
	float roughness{};
	float ao{};
	float emissiveStrength{};

	// Legacy (kept for batching stability with OpenGL fallback / old materials)
	float shininess{};
	float specStrength{};

	std::uint32_t permBits{};
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

			HashCombine(seed, HashU32(key.permBits));

			HashCombine(seed, HashU32(static_cast<std::uint32_t>(key.albedoDescIndex)));
			HashCombine(seed, HashU32(static_cast<std::uint32_t>(key.normalDescIndex)));
			HashCombine(seed, HashU32(static_cast<std::uint32_t>(key.metalnessDescIndex)));
			HashCombine(seed, HashU32(static_cast<std::uint32_t>(key.roughnessDescIndex)));
			HashCombine(seed, HashU32(static_cast<std::uint32_t>(key.aoDescIndex)));
			HashCombine(seed, HashU32(static_cast<std::uint32_t>(key.emissiveDescIndex)));

			HashCombine(seed, HashU32(FloatBits(key.baseColor.x)));
			HashCombine(seed, HashU32(FloatBits(key.baseColor.y)));
			HashCombine(seed, HashU32(FloatBits(key.baseColor.z)));
			HashCombine(seed, HashU32(FloatBits(key.baseColor.w)));

			HashCombine(seed, HashU32(FloatBits(key.shadowBias)));

			HashCombine(seed, HashU32(FloatBits(key.metallic)));
			HashCombine(seed, HashU32(FloatBits(key.roughness)));
			HashCombine(seed, HashU32(FloatBits(key.ao)));
			HashCombine(seed, HashU32(FloatBits(key.emissiveStrength)));

			// Legacy
			HashCombine(seed, HashU32(FloatBits(key.shininess)));
			HashCombine(seed, HashU32(FloatBits(key.specStrength)));
			return seed;
		}
	};

	struct BatchKeyEq
	{
		bool operator()(const BatchKey& lhs, const BatchKey& rhs) const noexcept
		{
			return lhs.mesh == rhs.mesh &&
				lhs.permBits == rhs.permBits &&
				lhs.albedoDescIndex == rhs.albedoDescIndex &&
				lhs.normalDescIndex == rhs.normalDescIndex &&
				lhs.metalnessDescIndex == rhs.metalnessDescIndex &&
				lhs.roughnessDescIndex == rhs.roughnessDescIndex &&
				lhs.aoDescIndex == rhs.aoDescIndex &&
				lhs.emissiveDescIndex == rhs.emissiveDescIndex &&
				lhs.baseColor == rhs.baseColor &&
				lhs.shadowBias == rhs.shadowBias &&
				lhs.metallic == rhs.metallic &&
				lhs.roughness == rhs.roughness &&
				lhs.ao == rhs.ao &&
				lhs.emissiveStrength == rhs.emissiveStrength &&
				lhs.shininess == rhs.shininess &&
				lhs.specStrength == rhs.specStrength;
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
        std::array<float, 4>  uCameraForward{}; // xyz + 0
		std::array<float, 4>  uBaseColor{};     // fallback baseColor

		// shininess, specStrength, materialShadowBiasTexels, flagsBits
		std::array<float, 4>  uMaterialFlags{};


		// metallic, roughness, ao, emissiveStrength
		std::array<float, 4>  uPbrParams{};
		// lightCount, spotShadowCount, pointShadowCount, unused
		std::array<float, 4>  uCounts{};

		// dirBaseTexels, spotBaseTexels, pointBaseTexels, slopeScaleTexels
		std::array<float, 4>  uShadowBias{};
	};
	static_assert(sizeof(PerBatchConstants) == 240);


	// shadow metadata for Spot/Point arrays (bound as StructuredBuffer at t11).
	// We pack indices/bias as floats to keep the struct simple across compilers.
	struct alignas(16) ShadowDataSB
	{
		// ---------------- Directional CSM (atlas) ----------------
		// Cascades are packed into a single D32 atlas.
		// Layout: [C0|C1|C2] horizontally, each tile is dirTileSize x dirTileSize.
		// dirVPRows: cascadeCount * 4 rows.
		std::array<mathUtils::Vec4, 12> dirVPRows{}; // 3 cascades * 4 rows
		// dirSplits = { split1, split2, split3 (max shadow distance), fadeFraction }
		mathUtils::Vec4 dirSplits{};
		// dirInfo = { invAtlasW, invAtlasH, invTileRes, cascadeCount }
		mathUtils::Vec4 dirInfo{};

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

			// ---------------- Directional CSM (atlas) ----------------
			// 3 cascades packed into a single D32 atlas:
			//   atlas = (tileSize * cascadeCount) x tileSize.
			// The shader selects the cascade and remaps UVs into the atlas.
			constexpr std::uint32_t kMaxDirCascades = 3;
			constexpr std::uint32_t dirTileSize = 2048; // user request
			const std::uint32_t dirCascadeCount = std::clamp(settings_.dirShadowCascadeCount, 1u, kMaxDirCascades);
			const rhi::Extent2D shadowExtent{ dirTileSize * dirCascadeCount, dirTileSize };
			const auto shadowRG = graph.CreateTexture(renderGraph::RGTextureDesc{
				.extent = shadowExtent,
				.format = rhi::Format::D32_FLOAT,
				.usage = renderGraph::ResourceUsage::DepthStencil,
				.debugName = "DirShadowAtlas"
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
			// Fit each cascade ortho projection to a camera frustum slice in light-space.
			// The bounds are snapped to shadow texels to reduce shimmering.
			const rhi::SwapChainDesc scDesc = swapChain.GetDesc();
			const float aspect = (scDesc.extent.height > 0)
				? (static_cast<float>(scDesc.extent.width) / static_cast<float>(scDesc.extent.height))
				: 1.0f;


			const mathUtils::Mat4 cameraProj = mathUtils::PerspectiveRH_ZO(mathUtils::DegToRad(scene.camera.fovYDeg), aspect, scene.camera.nearZ, scene.camera.farZ);
			const mathUtils::Mat4 cameraView = mathUtils::LookAt(scene.camera.position, scene.camera.target, scene.camera.up);
			const mathUtils::Mat4 cameraViewProj = cameraProj * cameraView;
			const mathUtils::Frustum cameraFrustum = mathUtils::ExtractFrustumRH_ZO(cameraViewProj);
			const bool doFrustumCulling = settings_.enableFrustumCulling;

			auto IsVisible = [&](const rendern::MeshResource* meshRes, const mathUtils::Mat4& model) -> bool
				{
					if (!doFrustumCulling || !meshRes)
					{
						return true;
					}
					const auto& b = meshRes->GetBounds();
					if (b.sphereRadius <= 0.0f)
					{
						return true;
					}

					const mathUtils::Vec4 wc4 = model * mathUtils::Vec4(b.sphereCenter, 1.0f);
					const mathUtils::Vec3 worldCenter{ wc4.x, wc4.y, wc4.z };

					const mathUtils::Vec3 c0{ model[0].x, model[0].y, model[0].z };
					const mathUtils::Vec3 c1{ model[1].x, model[1].y, model[1].z };
					const mathUtils::Vec3 c2{ model[2].x, model[2].y, model[2].z };
					const float s0 = mathUtils::Length(c0);
					const float s1 = mathUtils::Length(c1);
					const float s2 = mathUtils::Length(c2);
					const float maxScale = std::max(s0, std::max(s1, s2));
					const float worldRadius = b.sphereRadius * maxScale;

					return mathUtils::IntersectsSphere(cameraFrustum, worldCenter, worldRadius);
				};

			// Limit how far we render directional shadows to keep resolution usable.
			const float shadowFar = std::min(scene.camera.farZ, settings_.dirShadowDistance);
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

			// Cascade split distances (camera-space) using the "practical" split scheme.
			std::array<float, kMaxDirCascades + 1> dirSplits{};
			dirSplits[0] = shadowNear;
			dirSplits[dirCascadeCount] = shadowFar;
			for (std::uint32_t i = 1; i < dirCascadeCount; ++i)
			{
				const float p = static_cast<float>(i) / static_cast<float>(dirCascadeCount);
				const float logSplit = shadowNear * std::pow(shadowFar / shadowNear, p);
				const float uniSplit = shadowNear + (shadowFar - shadowNear) * p;
				dirSplits[i] = std::lerp(uniSplit, logSplit, settings_.dirShadowSplitLambda);
			}

			// Stable "up" for light view.
			const mathUtils::Vec3 worldUp(0.0f, 1.0f, 0.0f);
			const mathUtils::Vec3 lightUp = (std::abs(mathUtils::Dot(lightDir, worldUp)) > 0.99f)
				? mathUtils::Vec3(0.0f, 0.0f, 1.0f)
				: worldUp;

			// Build a view-proj per cascade.
			std::array<mathUtils::Mat4, kMaxDirCascades> dirCascadeVP{};
			for (std::uint32_t c = 0; c < dirCascadeCount; ++c)
			{
				const float cNear = dirSplits[c];
				const float cFar = dirSplits[c + 1];

				std::array<mathUtils::Vec3, 8> frustumCorners{};
				// Near plane
				frustumCorners[0] = MakeFrustumCorner(cNear, -1.0f, -1.0f);
				frustumCorners[1] = MakeFrustumCorner(cNear, 1.0f, -1.0f);
				frustumCorners[2] = MakeFrustumCorner(cNear, 1.0f, 1.0f);
				frustumCorners[3] = MakeFrustumCorner(cNear, -1.0f, 1.0f);
				// Far plane
				frustumCorners[4] = MakeFrustumCorner(cFar, -1.0f, -1.0f);
				frustumCorners[5] = MakeFrustumCorner(cFar, 1.0f, -1.0f);
				frustumCorners[6] = MakeFrustumCorner(cFar, 1.0f, 1.0f);
				frustumCorners[7] = MakeFrustumCorner(cFar, -1.0f, 1.0f);

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

				// Conservative padding in light-space to avoid hard clipping.
				const float extX = maxX - minX;
				const float extY = maxY - minY;
				const float extZ = maxZ - minZ;

				const float padXY = 0.05f * std::max(extX, extY) + 1.0f;
				const float padZ = 0.10f * extZ + 5.0f;
				minX -= padXY; maxX += padXY;
				minY -= padXY; maxY += padXY;
				minZ -= padZ;  maxZ += padZ;

				// Extra depth margin for casters outside the camera frustum (increases with cascade index).
				const float casterMargin = 20.0f + 30.0f * static_cast<float>(c);
				minZ -= casterMargin;

				// Snap the ortho window to shadow texels (reduces shimmering / popping).
				const float widthLS = maxX - minX;
				const float heightLS = maxY - minY;
				const float wuPerTexelX = widthLS / static_cast<float>(dirTileSize);
				const float wuPerTexelY = heightLS / static_cast<float>(dirTileSize);
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
				dirCascadeVP[c] = lightProj * lightView;
			}

			// For legacy constant-buffer field (kept for compatibility with older shaders).
			const mathUtils::Mat4 dirLightViewProj = dirCascadeVP[0];

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

				const mathUtils::Mat4 model = item.transform.ToMatrix();
				if (!IsVisible(item.mesh.get(), model))
				{
					continue;
				}

				BatchKey key{};
				key.mesh = mesh;

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

				key.permBits = static_cast<std::uint32_t>(perm);

				// IMPORTANT: BatchKey must include material parameters,
				// otherwise different materials get incorrectly merged.
				key.albedoDescIndex = params.albedoDescIndex;
				key.normalDescIndex = params.normalDescIndex;
				key.metalnessDescIndex = params.metalnessDescIndex;
				key.roughnessDescIndex = params.roughnessDescIndex;
				key.aoDescIndex = params.aoDescIndex;
				key.emissiveDescIndex = params.emissiveDescIndex;

				key.baseColor = params.baseColor;
				key.shadowBias = params.shadowBias; // texels

				key.metallic = params.metallic;
				key.roughness = params.roughness;
				key.ao = params.ao;
				key.emissiveStrength = params.emissiveStrength;

				// Legacy
				key.shininess = params.shininess;
				key.specStrength = params.specStrength;

				// Instance (ROWS)
				InstanceData inst{};
				inst.i0 = model[0];
				inst.i1 = model[1];
				inst.i2 = model[2];
				inst.i3 = model[3];

				const bool isTransparent = HasFlag(perm, MaterialPerm::Transparent) || (params.baseColor.w < 0.999f);
				if (isTransparent)
				{
					mathUtils::Vec3 sortPos = item.transform.position;
					const auto& b = item.mesh->GetBounds();
					if (b.sphereRadius > 0.0f)
					{
						const mathUtils::Vec4 wc4 = model * mathUtils::Vec4(b.sphereCenter, 1.0f);
						sortPos = mathUtils::Vec3(wc4.x, wc4.y, wc4.z);
					}
					else
					{
						sortPos = mathUtils::Vec3(model[3].x, model[3].y, model[3].z);
					}

					const mathUtils::Vec3 deltaToCamera = sortPos - camPos;
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
						<< ", shadow: " << shadowInstances.size() << ")"
						<< " | DepthPrepass: " << (settings_.enableDepthPrepass ? "ON" : "OFF")
						<< " (draw calls: " << shadowBatches.size() << ")\n";
				}
			}

			// ---------------- Create shadow passes (all reuse shadowBatches) ----------------
			// Directional CSM atlas (depth-only). We clear the whole atlas once, then render each cascade
			// into its own 2048x2048 viewport tile.
			for (std::uint32_t cascade = 0; cascade < dirCascadeCount; ++cascade)
			{
				rhi::ClearDesc clear{};
				clear.clearColor = false;
				clear.clearDepth = (cascade == 0u);
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
				const mathUtils::Mat4 vpT = mathUtils::Transpose(dirCascadeVP[cascade]);
				std::memcpy(shadowPassConstants.uLightViewProj.data(), mathUtils::ValuePtr(vpT), sizeof(float) * 16);

				const int vpX = static_cast<int>(cascade * dirTileSize);
				const int vpY = 0;
				const int vpW = static_cast<int>(dirTileSize);
				const int vpH = static_cast<int>(dirTileSize);

				const char* passName = (cascade == 0u) ? "DirShadow_C0" : (cascade == 1u) ? "DirShadow_C1" : "DirShadow_C2";
				graph.AddPass(passName, std::move(att),
					[this, shadowPassConstants, shadowBatches, instStride, vpX, vpY, vpW, vpH](renderGraph::PassContext& ctx) mutable
					{
						ctx.commandList.SetViewport(vpX, vpY, vpW, vpH);

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

				// Directional CSM (atlas)
				{
					// Store up to 3 cascades; shader reads only the first dirCascadeCount entries.
					for (std::uint32_t c = 0; c < dirCascadeCount; ++c)
					{
						// Mat4 is column-major (GLM convention). In the HLSL we multiply as `mul(v, M)`
						// (row-vector), so we want to feed the *transposed* matrix. To avoid an extra CPU transpose,
						// we pack the matrix columns and the shader reconstructs a float4x4 from them as rows.
						const mathUtils::Mat4& vp = dirCascadeVP[c];
						sd.dirVPRows[c * 4 + 0] = vp[0];
						sd.dirVPRows[c * 4 + 1] = vp[1];
						sd.dirVPRows[c * 4 + 2] = vp[2];
						sd.dirVPRows[c * 4 + 3] = vp[3];
					}

					const float split1 = (dirCascadeCount >= 2) ? dirSplits[1] : dirSplits[dirCascadeCount];
					const float split2 = (dirCascadeCount >= 3) ? dirSplits[2] : dirSplits[dirCascadeCount];
					const float split3 = dirSplits[dirCascadeCount];
					const float fadeFrac = 0.10f; // blend width as a fraction of cascade length
					sd.dirSplits = mathUtils::Vec4(split1, split2, split3, fadeFrac);

					const float invAtlasW = 1.0f / static_cast<float>(shadowExtent.width);
					const float invAtlasH = 1.0f / static_cast<float>(shadowExtent.height);
					const float invTile = 1.0f / static_cast<float>(dirTileSize);
					sd.dirInfo = mathUtils::Vec4(invAtlasW, invAtlasH, invTile, static_cast<float>(dirCascadeCount));
				}

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

			// ---------------- Optional depth pre-pass (swapchain depth) ----------------
			const bool doDepthPrepass = settings_.enableDepthPrepass;
			if (doDepthPrepass && psoShadow_)
			{
				// We use the existing depth-only shadow shader (writes SV_Depth, no color outputs).
				// It expects a single matrix (uLightViewProj), so we feed it the camera view-projection.
				rhi::ClearDesc preClear{};
				preClear.clearColor = false; // keep backbuffer untouched
				preClear.clearDepth = true;
				preClear.depth = 1.0f;

				graph.AddSwapChainPass("PreDepthPass", preClear,
					[this, &scene, shadowBatches, instStride](renderGraph::PassContext& ctx) mutable
					{
						const auto extent = ctx.passExtent;
						ctx.commandList.SetViewport(0, 0,
							static_cast<int>(extent.width),
							static_cast<int>(extent.height));

						// Pre-depth state: depth test+write, opaque raster.
						ctx.commandList.SetState(preDepthState_);
						ctx.commandList.BindPipeline(psoShadow_);

						const float aspect = extent.height
							? (static_cast<float>(extent.width) / static_cast<float>(extent.height))
							: 1.0f;

						const mathUtils::Mat4 proj = mathUtils::PerspectiveRH_ZO(
							mathUtils::DegToRad(scene.camera.fovYDeg),
							aspect,
							scene.camera.nearZ,
							scene.camera.farZ);
						const mathUtils::Mat4 view = mathUtils::LookAt(scene.camera.position, scene.camera.target, scene.camera.up);
						const mathUtils::Mat4 viewProj = proj * view;

						struct alignas(16) PreDepthConstants
						{
							std::array<float, 16> uLightViewProj{};
						};
						PreDepthConstants c{};
						const mathUtils::Mat4 vpT = mathUtils::Transpose(viewProj);
						std::memcpy(c.uLightViewProj.data(), mathUtils::ValuePtr(vpT), sizeof(float) * 16);
						ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &c, 1 }));

						this->DrawInstancedShadowBatches(ctx.commandList, shadowBatches, instStride);
					});
			}

			// ---------------- Main pass (swapchain) ----------------
			rhi::ClearDesc clearDesc{};
			clearDesc.clearColor = true;
			clearDesc.clearDepth = !doDepthPrepass; // if we pre-filled depth, don't wipe it here
			clearDesc.color = { 0.1f, 0.1f, 0.1f, 1.0f };

			graph.AddSwapChainPass("MainPass", clearDesc,
				[this, &scene,
				shadowRG,
				dirLightViewProj,
				lightCount,
				spotShadows,
				pointShadows,
				mainBatches,
				instStride,
				transparentDraws,
				doDepthPrepass,
				imguiDrawData](renderGraph::PassContext& ctx)
				{
					const auto extent = ctx.passExtent;

					ctx.commandList.SetViewport(0, 0,
						static_cast<int>(extent.width),
						static_cast<int>(extent.height));

					// If we ran a depth prepass, keep depth read-only in the main pass.
					ctx.commandList.SetState(doDepthPrepass ? mainAfterPreDepthState_ : state_);

					const float aspect = extent.height
						? (static_cast<float>(extent.width) / static_cast<float>(extent.height))
						: 1.0f;

					const mathUtils::Mat4 proj = mathUtils::PerspectiveRH_ZO(mathUtils::DegToRad(scene.camera.fovYDeg), aspect, scene.camera.nearZ, scene.camera.farZ);
					const mathUtils::Mat4 view = mathUtils::LookAt(scene.camera.position, scene.camera.target, scene.camera.up);
					const mathUtils::Vec3 camPosLocal = scene.camera.position;

					const mathUtils::Vec3 camFLocal = mathUtils::Normalize(scene.camera.target - scene.camera.position);
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

							ctx.commandList.SetState(doDepthPrepass ? mainAfterPreDepthState_ : state_);
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
					constexpr std::uint32_t kFlagUseNormal = 1u << 2;
					constexpr std::uint32_t kFlagUseMetalTex = 1u << 3;
					constexpr std::uint32_t kFlagUseRoughTex = 1u << 4;
					constexpr std::uint32_t kFlagUseAOTex = 1u << 5;
					constexpr std::uint32_t kFlagUseEmissiveTex = 1u << 6;
					constexpr std::uint32_t kFlagUseEnv = 1u << 7;

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
						ctx.commandList.BindTextureDesc(12, batch.material.normalDescIndex);
						ctx.commandList.BindTextureDesc(13, batch.material.metalnessDescIndex);
						ctx.commandList.BindTextureDesc(14, batch.material.roughnessDescIndex);
						ctx.commandList.BindTextureDesc(15, batch.material.aoDescIndex);
						ctx.commandList.BindTextureDesc(16, batch.material.emissiveDescIndex);
						ctx.commandList.BindTextureDesc(17, scene.skyboxDescIndex);

						std::uint32_t flags = 0;
						if (useTex)
						{
							flags |= kFlagUseTex;
						}
						if (useShadow)
						{
							flags |= kFlagUseShadow;
						}
						if (batch.material.normalDescIndex != 0)
						{
							flags |= kFlagUseNormal;
						}
						if (batch.material.metalnessDescIndex != 0)
						{
							flags |= kFlagUseMetalTex;
						}
						if (batch.material.roughnessDescIndex != 0)
						{
							flags |= kFlagUseRoughTex;
						}
						if (batch.material.aoDescIndex != 0)
						{
							flags |= kFlagUseAOTex;
						}
						if (batch.material.emissiveDescIndex != 0)
						{
							flags |= kFlagUseEmissiveTex;
						}
						if (scene.skyboxDescIndex != 0)
						{
							flags |= kFlagUseEnv;
						}
						if (batch.material.normalDescIndex != 0)
						{
							flags |= kFlagUseNormal;
						}
						if (batch.material.metalnessDescIndex != 0)
						{
							flags |= kFlagUseMetalTex;
						}
						if (batch.material.roughnessDescIndex != 0)
						{
							flags |= kFlagUseRoughTex;
						}
						if (batch.material.aoDescIndex != 0)
						{
							flags |= kFlagUseAOTex;
						}
						if (batch.material.emissiveDescIndex != 0)
						{
							flags |= kFlagUseEmissiveTex;
						}
						if (scene.skyboxDescIndex != 0)
						{
							flags |= kFlagUseEnv;
						}

						PerBatchConstants constants{};
						const mathUtils::Mat4 viewProjT = mathUtils::Transpose(viewProj);
						const mathUtils::Mat4 dirVP_T = mathUtils::Transpose(dirLightViewProj);

						std::memcpy(constants.uViewProj.data(), mathUtils::ValuePtr(viewProjT), sizeof(float) * 16);
						std::memcpy(constants.uLightViewProj.data(), mathUtils::ValuePtr(dirVP_T), sizeof(float) * 16);

						constants.uCameraAmbient = { camPosLocal.x, camPosLocal.y, camPosLocal.z, 0.22f };
						constants.uCameraForward = { camFLocal.x, camFLocal.y, camFLocal.z, 0.0f };
						constants.uBaseColor = { batch.material.baseColor.x, batch.material.baseColor.y, batch.material.baseColor.z, batch.material.baseColor.w };

						const float materialBiasTexels = batch.material.shadowBias;
						constants.uMaterialFlags = { 0.0f, 0.0f, materialBiasTexels, AsFloatBits(flags) };

						constants.uPbrParams = { batch.material.metallic, batch.material.roughness, batch.material.ao, batch.material.emissiveStrength };

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
							ctx.commandList.BindTextureDesc(12, batchTransparent.material.normalDescIndex);
							ctx.commandList.BindTextureDesc(13, batchTransparent.material.metalnessDescIndex);
							ctx.commandList.BindTextureDesc(14, batchTransparent.material.roughnessDescIndex);
							ctx.commandList.BindTextureDesc(15, batchTransparent.material.aoDescIndex);
							ctx.commandList.BindTextureDesc(16, batchTransparent.material.emissiveDescIndex);
							ctx.commandList.BindTextureDesc(17, scene.skyboxDescIndex);

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
							constants.uCameraForward = { camFLocal.x, camFLocal.y, camFLocal.z, 0.0f };
							constants.uBaseColor = { batchTransparent.material.baseColor.x, batchTransparent.material.baseColor.y, batchTransparent.material.baseColor.z, batchTransparent.material.baseColor.w };

							const float materialBiasTexels = batchTransparent.material.shadowBias;
							constants.uMaterialFlags = { 0.0f, 0.0f, materialBiasTexels, AsFloatBits(flags) };

							constants.uPbrParams = { batchTransparent.material.metallic, batchTransparent.material.roughness, batchTransparent.material.ao, batchTransparent.material.emissiveStrength };

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

				// Depth pre-pass state: same raster as opaque, depth test+write enabled.
				preDepthState_ = state_;

				// Main pass state when running after a depth pre-pass: keep depth read-only.
				mainAfterPreDepthState_ = state_;
				mainAfterPreDepthState_.depth.writeEnable = false;
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
		rhi::GraphicsState preDepthState_{};
		rhi::GraphicsState mainAfterPreDepthState_{};

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
