module;

#include <filesystem>

// Shared, backend-agnostic renderer settings.
export module core:renderer_settings;

export namespace rendern
{
	struct RendererSettings
	{
		float dirShadowBaseBiasTexels{ 0.6f };
		float spotShadowBaseBiasTexels{ 1.0f };
		float pointShadowBaseBiasTexels{ 3.0f };
		float shadowSlopeScaleTexels{ 2.0f };

		// Directional shadow cascade settings (DX12-only usage; safe to ignore in other backends)
		float dirShadowDistance{ 200.0f };
		std::uint32_t dirShadowCascadeCount{ 3 };
		float dirShadowSplitLambda{ 0.7f };
		bool enableDepthPrepass{ false };
		bool enableFrustumCulling{ true };
		bool debugPrintDrawCalls{ false }; // prints MainPass draw-call count (DX12) once per ~60 frames

		bool drawLightGizmos{ true };
		float lightGizmoHalfSize{ 0.15f };
		float debugLightGizmoScale = 1.0f;
		float lightGizmoArrowLength{ 1.5f };
		float lightGizmoArrowThickness{ 0.05f };

		std::filesystem::path modelPath = std::filesystem::path("models") / "cube.obj";
	};
}
