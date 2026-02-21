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
		bool reflectionCaptureFollowSelectedObject{ true };
		std::uint32_t reflectionCaptureResolution{ 256 }; // cube face size (px)
		float reflectionCaptureNearZ{ 0.05f };
		float reflectionCaptureFarZ{ 200.0f };

		bool drawLightGizmos{ true };
		bool debugDrawDepthTest{ true };
		float lightGizmoHalfSize{ 0.15f };
		float debugLightGizmoScale = 1.0f;
		float lightGizmoArrowLength{ 1.5f };
		float lightGizmoArrowThickness{ 0.05f };

		bool ShowCubeAtlas{ false }; // debug: visualize point shadow cubemap atlas on the swapchain (DX12-only)
		float debugCubeAtlasIndex{ 0.0f };

		std::filesystem::path modelPath = std::filesystem::path("models") / "cube.obj";
	};
}
