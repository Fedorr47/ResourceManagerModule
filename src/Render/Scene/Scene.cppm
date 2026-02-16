module;

#include <cstdint>
#include <vector>
#include <span>
#include <stdexcept>
#include <memory>

export module core:scene;

import :rhi;
import :resource_manager_mesh;
import :math_utils;

export namespace rendern
{
	// High-level transform used by the CPU side.
	// Convention: rotationDegrees is applied as Z * Y * X after translation.
	struct Transform
	{
		mathUtils::Vec3 position{ 0.0f, 0.0f, 0.0f };
		mathUtils::Vec3 rotationDegrees{ 0.0f, 0.0f, 0.0f };
		mathUtils::Vec3 scale{ 1.0f, 1.0f, 1.0f };

		// Optional: allow importing transforms directly as a matrix (e.g. from DCC tools).
		// Matrix is COLUMN-major and follows the same convention as mathUtils::Mat4 (m[col][row]).
		bool useMatrix{ false };
		mathUtils::Mat4 matrix{ 1.0f };

		mathUtils::Mat4 ToMatrix() const
		{
			if (useMatrix)
			{
				return matrix;
			}
			mathUtils::Mat4 m{ 1.0f };
			m = mathUtils::Translate(m, position);
			m = mathUtils::Rotate(m, mathUtils::DegToRad(rotationDegrees.z), mathUtils::Vec3(0, 0, 1));
			m = mathUtils::Rotate(m, mathUtils::DegToRad(rotationDegrees.y), mathUtils::Vec3(0, 1, 0));
			m = mathUtils::Rotate(m, mathUtils::DegToRad(rotationDegrees.x), mathUtils::Vec3(1, 0, 0));
			m = mathUtils::Scale(m, scale);
			return m;
		}
	};

	struct Camera
	{
		mathUtils::Vec3 position{ 2.2f, 1.6f, 2.2f };
		mathUtils::Vec3 target{ 0.0f, 0.0f, 0.0f };
		mathUtils::Vec3 up{ 0.0f, 1.0f, 0.0f };

		float fovYDeg{ 60.0f };
		float nearZ{ 0.01f };
		float farZ{ 200.0f };
	};
	enum class LightType : std::uint32_t
	{
		Directional = 0,
		Point = 1,
		Spot = 2
	};

	// CPU-side light description.
	// direction is "FROM light towards the scene" for Directional and Spot.
	struct Light
	{
		LightType type{ LightType::Directional };

		mathUtils::Vec3 position{ 0.0f, 0.0f, 0.0f };
		mathUtils::Vec3 direction{ 0.0f, -1.0f, 0.0f };

		mathUtils::Vec3 color{ 1.0f, 1.0f, 1.0f };
		float intensity{ 1.0f };

		float range{ 10.0f };
		float innerHalfAngleDeg{ 12.0f };
		float outerHalfAngleDeg{ 20.0f };

		float attConstant{ 1.0f };
		float attLinear{ 0.12f };
		float attQuadratic{ 0.04f };
	};

	// Debug visualization data (runtime-only).
	struct DebugRay
	{
		bool enabled{ false };
		mathUtils::Vec3 origin{ 0.0f, 0.0f, 0.0f };
		mathUtils::Vec3 direction{ 0.0f, 0.0f, 1.0f }; // should be normalized
		float length{ 0.0f };
		bool hit{ false };
	};

	struct MaterialParams
	{
		mathUtils::Vec4 baseColor{ 1.0f, 1.0f, 1.0f, 1.0f };

		// Legacy Phong params (still used by OpenGL path).
		float shininess{ 64.0f };
		float specStrength{ 0.5f };

		// Shadow bias in "texels" (added to the global bias computed in shader).
		float shadowBias{ 0.0f };

		// --- PBR params (DX12 path) ---
		// Defaults are chosen to look reasonable even when only albedo is provided.
		float metallic{ 0.0f };   // 0..1
		float roughness{ 0.75f }; // 0..1
		float ao{ 1.0f };         // 0..1
		float emissiveStrength{ 1.0f };

		// Cross-backend binding: if non-zero, renderer binds this descriptor at slot t0.
		rhi::TextureDescIndex albedoDescIndex{ 0 };

		// DX12 PBR maps (bound as separate SRV slots in the main shader):
		//  t12 normal, t13 metalness, t14 roughness, t15 ao, t16 emissive
		rhi::TextureDescIndex normalDescIndex{ 0 };
		rhi::TextureDescIndex metalnessDescIndex{ 0 };
		rhi::TextureDescIndex roughnessDescIndex{ 0 };
		rhi::TextureDescIndex aoDescIndex{ 0 };
		rhi::TextureDescIndex emissiveDescIndex{ 0 };
	};

	enum class MaterialPerm : std::uint32_t
	{
		None		= 0,
		UseTex		= 1u << 0,
		UseShadow	= 1u << 1,
		Skinning	= 1u << 2,
		Transparent = 1u << 3
	};

	constexpr MaterialPerm operator|(MaterialPerm a, MaterialPerm b) noexcept
	{
		return static_cast<MaterialPerm>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
	}
	constexpr MaterialPerm operator&(MaterialPerm a, MaterialPerm b) noexcept
	{
		return static_cast<MaterialPerm>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
	}
	constexpr MaterialPerm& operator|=(MaterialPerm& a, MaterialPerm b) noexcept
	{
		a = a | b;
		return a;
	}
	constexpr bool HasFlag(MaterialPerm a, MaterialPerm b) noexcept
	{
		return static_cast<std::uint32_t>(a & b) != 0;
	}

	struct MaterialTag {};
	using MaterialHandle = rhi::Handle<MaterialTag>;
	using MeshHandle = std::shared_ptr<MeshResource>;
	// Material = "how we draw": parameters + textures + permutation flags.
	// NOTE: UseTex is inferred automatically if albedoDescIndex != 0.
	struct Material
	{
		MaterialParams params{};
		MaterialPerm permFlags{ MaterialPerm::UseShadow };
	};

	inline MaterialPerm EffectivePerm(const Material& material) noexcept
	{
		MaterialPerm maetrialPerm = material.permFlags;
		// a < 1 => transparent even if flag isn't set.
		if (material.params.baseColor.w < 0.999f)
		{
			maetrialPerm |= MaterialPerm::Transparent;
		}
		if (material.params.albedoDescIndex != 0)
		{
			maetrialPerm |= MaterialPerm::UseTex;
		}
		return maetrialPerm;
	}

	struct DrawItem
	{
		// Scene owns only a handle. Upload/Destroy are driven by Asset/ResourceManager.
		// Renderer will skip items whose mesh hasn't finished loading / uploading.
		MeshHandle mesh{};
		Transform transform{};
		MaterialHandle material{};
	};

	class Scene
	{
	public:
		rendern::Camera camera{};
		std::vector<Material> materials; // persistent "assets" owned by Scene
		std::vector<DrawItem> drawItems;
		std::vector<Light> lights;

		rhi::TextureDescIndex skyboxDescIndex{ 0 };

		DebugRay debugPickRay{};

		// Editor selection (runtime-only). Index into LevelAsset::nodes.
		int editorSelectedNode{ -1 };

		void Clear()
		{
			drawItems.clear();
			lights.clear();
			skyboxDescIndex = 0;
			debugPickRay = {};
			editorSelectedNode = -1;

		}

		MaterialHandle CreateMaterial(const Material& m)
		{
			materials.push_back(m);
			return MaterialHandle{ static_cast<std::uint32_t>(materials.size()) }; // id is 1-based
		}

		const Material& GetMaterial(MaterialHandle h) const
		{
			if (h.id == 0 || h.id > materials.size())
			{
				throw std::runtime_error("Scene::GetMaterial: invalid MaterialHandle");
			}
			return materials[h.id - 1];
		}

		Material& GetMaterial(MaterialHandle h)
		{
			if (h.id == 0 || h.id > materials.size())
			{
				throw std::runtime_error("Scene::GetMaterial: invalid MaterialHandle");
			}
			return materials[h.id - 1];
		}

		DrawItem& AddDraw(const DrawItem& item)
		{
			drawItems.push_back(item);
			return drawItems.back();
		}

		Light& AddLight(const Light& l)
		{
			lights.push_back(l);
			return lights.back();
		}

		std::span<const Material> GetMaterials() const { return materials; }
		std::span<Material> GetMaterials() { return materials; }

		std::span<const DrawItem> GetDrawItems() const { return drawItems; }
		std::span<DrawItem> GetDrawItems() { return drawItems; }

		std::span<const Light> GetLights() const { return lights; }
		std::span<Light> GetLights() { return lights; }
	};
} // namespace rendern
