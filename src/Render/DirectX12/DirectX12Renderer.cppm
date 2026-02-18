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
import :debug_draw;
import :debug_draw_renderer_dx12;

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
			, debugDrawRenderer_(device, shaderLibrary_, psoCache_)
		{
			CreateResources();
		}

		void SetSettings(const RendererSettings& settings)
		{
			settings_ = settings;
		}

		void RenderFrame(rhi::IRHISwapChain& swapChain, const Scene& scene, const void* imguiDrawData)
		{
			#include "RendererImpl/DirectX12Renderer_RenderFrame_00_SetupCSM.inl"
			#include "RendererImpl/DirectX12Renderer_RenderFrame_01_BuildInstances.inl"
			#include "RendererImpl/DirectX12Renderer_RenderFrame_02_ShadowPasses.inl"
			#include "RendererImpl/DirectX12Renderer_RenderFrame_03_PreDepth.inl"
			#include "RendererImpl/DirectX12Renderer_RenderFrame_04_MainPass.inl"
			#include "RendererImpl/DirectX12Renderer_RenderFrame_05_DebugAndPresent.inl"
		}

		void Shutdown()
		{
			#include "RendererImpl/DirectX12Renderer_Shutdown.inl"
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
			#include "RendererImpl/DirectX12Renderer_UploadLights.inl"
		}

		void CreateResources()
		{
			#include "RendererImpl/DirectX12Renderer_CreateResources_00_PathsSkybox.inl"
			#include "RendererImpl/DirectX12Renderer_CreateResources_01_MainPipelines.inl"
			#include "RendererImpl/DirectX12Renderer_CreateResources_02_ShadowPipelines.inl"
			#include "RendererImpl/DirectX12Renderer_CreateResources_03_DynamicBuffers.inl"
		}

	private:
		static constexpr std::uint32_t kMaxLights = 64;
		static constexpr std::uint32_t kDefaultInstanceBufferSizeBytes = 8u * 1024u * 1024u; // 8 MB (combined shadow+main instances)

		rhi::IRHIDevice& device_;
		RendererSettings settings_{};

		ShaderLibrary shaderLibrary_;
		PSOCache psoCache_;
		debugDraw::DebugDrawRendererDX12 debugDrawRenderer_;

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
		rhi::PipelineHandle psoPointShadowLayered_{}; // SM6.1 + SV_RenderTargetArrayIndex layered rendering (single-pass cubemap)
		bool disablePointShadowLayered_{ false };// do not try again after first failure (until restart)
		rhi::PipelineHandle psoPointShadowVI_{}; // SM6.1 + SV_ViewID + view instancing (single-pass cubemap)
		bool disablePointShadowVI_{ false };// do not try again after first failure (until restart)
		rhi::GraphicsState pointShadowState_{};

		MeshRHI skyboxMesh_{};
		rhi::PipelineHandle psoSkybox_{};
		rhi::GraphicsState skyboxState_{};
	};
} // namespace rendern
