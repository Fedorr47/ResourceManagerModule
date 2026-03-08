module;

export module core:common_DX12_Structs;

import std;
import :mesh;
import :rhi;
import :math_utils;
import :scene;
import :render_graph;

export namespace rendern
{
	// multiple Spot/Point shadow casters (DX12). Keep small caps for now.
	constexpr std::uint32_t kMaxSpotShadows = 4;
	constexpr std::uint32_t kMaxPointShadows = 4;

	struct DeferredReflectionProbeGpu
	{
		std::array<float, 4> boxMin;
		std::array<float, 4> boxMax;
		std::array<float, 4> capturePosDesc;
	};
	static_assert(sizeof(DeferredReflectionProbeGpu) == 48);

	struct alignas(16) GPULight
	{
		std::array<float, 4> p0{}; // pos.xyz, type
		std::array<float, 4> p1{}; // dir.xyz (FROM light), intensity
		std::array<float, 4> p2{}; // color.rgb, range
		std::array<float, 4> p3{}; // cosInner, cosOuter, attLin, attQuad
	};

	struct alignas(16) ReflectionCaptureConstants
	{
		// 6 matrices * 4 rows = 24 float4 rows = 96 floats = 384 bytes
		std::array<float, 16u * 6u> uFaceViewProj{};   // row-major rows for each face (4 rows per face)

		std::array<float, 4> uCapturePosAmbient{};     // xyz + ambientStrength
		std::array<float, 4> uBaseColor{};             // rgba
		std::array<float, 4> uParams{};                // x=lightCount, y=flagsBits(asfloat), z,w unused
	};
	static_assert(sizeof(ReflectionCaptureConstants) <= 512);

	struct alignas(16) ReflectionCaptureFaceConstants
	{
		std::array<float, 16> uViewProj{};             // 4 rows
		std::array<float, 4>  uCapturePosAmbient{};    // xyz + ambientStrength
		std::array<float, 4>  uBaseColor{};            // rgba
		std::array<float, 4>  uParams{};               // x=lightCount, y=flagsBits(asfloat), z,w unused
	};
	static_assert(sizeof(ReflectionCaptureFaceConstants) <= 512);

	struct alignas(16) SingleMatrixPassConstants
	{
		std::array<float, 16> uLightViewProj{};
	};

	struct alignas(16) PointShadowCubeConstants
	{
		std::array<float, 16 * 6> uFaceViewProj{};
		std::array<float, 4> uLightPosRange{};
		std::array<float, 4> uMisc{};
	};

	struct alignas(16) PointShadowFaceConstants
	{
		std::array<float, 16> uFaceViewProj{};
		std::array<float, 4> uLightPosRange{};
		std::array<float, 4> uMisc{};
	};

	struct InstanceData
	{
		mathUtils::Vec4 i0; // column 0 of model
		mathUtils::Vec4 i1; // column 1
		mathUtils::Vec4 i2; // column 2
		mathUtils::Vec4 i3; // column 3
	};
	static_assert(sizeof(InstanceData) == 64);

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

	struct PlanarMirrorDraw
	{
		const rendern::MeshRHI* mesh{};
		MaterialParams material{};
		MaterialHandle materialHandle{};
		std::uint32_t instanceOffset{ 0 }; // absolute offset in combined instance buffer
		mathUtils::Vec3 planePoint{ 0.0f, 0.0f, 0.0f };
		mathUtils::Vec3 planeNormal{ 0.0f, 1.0f, 0.0f };
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

	struct ReflectionProbeRuntime
	{
		int ownerDrawItem = -1;
		mathUtils::Vec3 capturePos{};
		bool dirty = true;
		bool hasLastPos = false;
		mathUtils::Vec3 lastPos{};
		rhi::TextureHandle cube{};
		rhi::TextureHandle depthCube{};
		rhi::TextureDescIndex cubeDescIndex{};
	};

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
		rhi::TextureDescIndex specularDescIndex{};
		rhi::TextureDescIndex glossDescIndex{};

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
		std::uint32_t envSource{};
		int reflectionProbeIndex = -1;
	};

	struct BatchKeyEq
	{
		bool operator()(const BatchKey& lhs, const BatchKey& rhs) const noexcept
		{
			return lhs.mesh == rhs.mesh &&
				lhs.permBits == rhs.permBits &&
				lhs.envSource == rhs.envSource &&
				lhs.reflectionProbeIndex == rhs.reflectionProbeIndex &&
				lhs.albedoDescIndex == rhs.albedoDescIndex &&
				lhs.normalDescIndex == rhs.normalDescIndex &&
				lhs.metalnessDescIndex == rhs.metalnessDescIndex &&
				lhs.roughnessDescIndex == rhs.roughnessDescIndex &&
				lhs.aoDescIndex == rhs.aoDescIndex &&
				lhs.emissiveDescIndex == rhs.emissiveDescIndex &&
				lhs.specularDescIndex == rhs.specularDescIndex &&
				lhs.glossDescIndex == rhs.glossDescIndex &&
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
		int reflectionProbeIndex = -1;
		std::vector<InstanceData> inst;
	};

	struct Batch
	{
		const rendern::MeshRHI* mesh{};
		MaterialParams material{};
		MaterialHandle materialHandle{};
		std::uint32_t instanceOffset = 0; // in instances[]
		std::uint32_t instanceCount = 0;
		int reflectionProbeIndex = -1;
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

		// xyz = probe capture position, w = box half-extent (world units).
		// Used for parallax-corrected (box-projected) reflection probes when sampling dynamic env cubemaps.
		std::array<float, 4> uEnvProbeBoxMin{};
		std::array<float, 4> uEnvProbeBoxMax{};

		// Bindless material texture descriptor indices (DX12 SM6 deferred).
		// Packed as float4 to keep the constant buffer simple across backends.
		// x=albedo, y=normal, z=metalness, w=roughness
		std::array<float, 4> uTexIndices0{};
		// x=ao, y=emissive, z,w unused
		std::array<float, 4> uTexIndices1{};
	};
	static_assert(sizeof(PerBatchConstants) == 304);

	struct EditorSelectionDraw
	{
		const rendern::MeshRHI* mesh{};
		InstanceData instance{};
		float outlineWorldOffset = 0.025f;
		bool isTransparent = false;
	};

	struct SSAOConstants
	{
		std::array<float, 16> uInvViewProj{};
		mathUtils::Vec4 uParams{};  // radius, bias, strength, power
		mathUtils::Vec4 uInvSize{}; // 1/w, 1/h, 0,0
	};
	static_assert(sizeof(SSAOConstants) % 16 == 0);

	struct SSAOBlurConstants
	{
		mathUtils::Vec4 uInvSize{};
		mathUtils::Vec4 uParams{}; // depthThreshold
	};
	static_assert(sizeof(SSAOBlurConstants) % 16 == 0);

	struct alignas(16) FogConstants
	{
		std::array<float, 16> uInvViewProj{};
		std::array<float, 4> uCameraPos{}; // xyz + pad
		std::array<float, 4> uFogParams{}; // start, end, density, mode
		std::array<float, 4> uFogColor{};  // rgb + enabled(0/1)
	};
	static_assert(sizeof(FogConstants) % 16 == 0);

	struct alignas(16) BloomExtractConstants
	{
		mathUtils::Vec4 uInvSourceSize{}; // 1/w, 1/h
		mathUtils::Vec4 uParams{}; // threshold, softKnee, clampMax, pad
	};
	static_assert(sizeof(BloomExtractConstants) % 16 == 0);

	struct alignas(16) BloomBlurConstants
	{
		mathUtils::Vec4 uInvSourceSize{}; // 1/w, 1/h
		mathUtils::Vec4 uDirection{};     // x/y axis * radius
	};
	static_assert(sizeof(BloomBlurConstants) % 16 == 0);

	struct alignas(16) BloomCompositeConstants
	{
		mathUtils::Vec4 uParams{}; // intensity
	};
	static_assert(sizeof(BloomCompositeConstants) % 16 == 0);

	struct alignas(16) ToneMapConstants
	{
		mathUtils::Vec4 uParams{}; // exposure, mode, gamma, enableHDR
	};
	static_assert(sizeof(ToneMapConstants) % 16 == 0);

	struct FrameCameraData
	{
		mathUtils::Mat4 proj{ 1.0f };
		mathUtils::Mat4 view{ 1.0f };
		mathUtils::Mat4 viewProj{ 1.0f };
		mathUtils::Mat4 invViewProj{ 1.0f };
		mathUtils::Mat4 invViewProjT{ 1.0f };
		mathUtils::Vec3 camPos{ 0.0f, 0.0f, 0.0f };
		mathUtils::Vec3 camForward{ 0.0f, 0.0f, -1.0f };
	};

	struct ResolvedMaterialEnvBinding
	{
		rhi::TextureDescIndex descIndex{};
		rhi::TextureHandle arrayTexture{};
		bool usingReflectionProbeEnv = false;
	};

	struct alignas(16) DeferredLightingConstants
	{
		std::array<float, 16> uInvViewProj{};      // transpose(invViewProj)
		std::array<float, 4>  uCameraPosAmbient{}; // xyz + ambientStrength
		std::array<float, 4>  uCameraForward{};    // xyz + pad
		std::array<float, 4>  uShadowBias{};       // x=dirBaseBiasTexels, y=spotBaseBiasTexels, z=pointBaseBiasTexels, w=slopeScaleTexels
		std::array<float, 4>  uCounts{};           // x = lightCount, y = spotShadowCount, z = pointShadowCount, w = activeReflectionProbeCount
	};
	static_assert(sizeof(DeferredLightingConstants) == 128);
}