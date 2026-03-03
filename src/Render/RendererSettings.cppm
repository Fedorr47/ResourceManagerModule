module;

#include <filesystem>
#include <cstdint>

// Shared, backend-agnostic renderer settings.
export module core:renderer_settings;

export namespace rendern
{
	struct RendererSettings
	{
		float dirShadowBaseBiasTexels{ 0.6f };
		float spotShadowBaseBiasTexels{ 1.0f };
		float pointShadowBaseBiasTexels{ 0.4f };
		float shadowSlopeScaleTexels{ 2.0f };

		// Directional shadow cascade settings (DX12-only usage; safe to ignore in other backends)
		float dirShadowDistance{ 200.0f };
		std::uint32_t dirShadowCascadeCount{ 3 };
		float dirShadowSplitLambda{ 0.7f };
		bool enableDepthPrepass{ false };
		bool enableFrustumCulling{ true };
		bool debugPrintDrawCalls{ false }; // prints MainPass draw-call count (DX12) once per ~60 frames

		// Reflection capture (cubemap). Currently used by DX12 backend.
		bool enableReflectionCapture{ true };
		bool reflectionCaptureUpdateEveryFrame{ true };
		bool reflectionCaptureFollowSelectedObject{ false };
		std::uint32_t reflectionCaptureResolution{ 1024 }; // cube face size (px)
		float reflectionCaptureNearZ{ 0.05f };
		float reflectionCaptureFarZ{ 200.0f };

		// Parallax-corrected (box-projected) reflection probes: box half-extent in world units.
		// 0 disables box projection (falls back to direction-only env sampling).
		float reflectionProbeBoxHalfExtent{ 10.0f };

		// Planar reflections (DX12 MVP): mark mirror materials with MaterialPerm::PlanarMirror.
		bool enablePlanarReflections{ true };
		std::uint32_t planarReflectionMaxMirrors{ 1 };

		bool drawLightGizmos{ false };
		bool debugDrawDepthTest{ true };
		float lightGizmoHalfSize{ 0.15f };
		float debugLightGizmoScale = 1.0f;
		float lightGizmoArrowLength{ 1.5f };
		float lightGizmoArrowThickness{ 0.05f };

		bool ShowCubeAtlas{ false }; // debug: visualize point shadow cubemap atlas on the swapchain (DX12-only)
		std::uint32_t debugCubeAtlasIndex{ 0 };
		std::uint32_t debugShadowCubeMapType{ 1 }; // point shadow: cube/light index; reflection mode: reflective-owner index

		bool drawPlanarMirrorNormals{ false };
		float planarMirrorNormalLength{ 2.0f };

		bool loadingOverlayVisible{ false };
		float loadingOverlayProgressBar{ 0.0f };

		std::uint32_t loadingOverlayTotalUnits{ 0u };
		std::uint32_t loadingOverlayCompletedUnits{ 0u };

		float reflectionCaptureFovPadDeg{ 0.0f };
		std::filesystem::path modelPath = std::filesystem::path("models") / "cube.obj";
	};
}
