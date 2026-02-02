module;

#include <filesystem>

// Shared, backend-agnostic renderer settings.
export module core:renderer_settings;

export namespace rendern
{
	struct RendererSettings
	{
		float dirShadowBaseBiasTexels{ 1.0f };
		float spotShadowBaseBiasTexels{ 0.6f };
		float pointShadowBaseBiasTexels{ 0.8f };
		float shadowSlopeScaleTexels{ 1.0f };

		bool enableDepthPrepass{ false };
		bool debugPrintDrawCalls{ true }; // prints MainPass draw-call count (DX12) once per ~60 frames
		std::filesystem::path modelPath = std::filesystem::path("models") / "cube.obj";
	};
}
