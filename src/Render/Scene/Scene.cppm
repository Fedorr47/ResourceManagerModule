module;

#include <cstdint>
#include <vector>
#include <span>
#include <stdexcept>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

export module core:scene;

import :rhi;
import :mesh;

export namespace rendern
{
	// High-level transform used by the CPU side.
	// Convention: rotationDegrees is applied as Z * Y * X after translation.
	struct Transform
	{
		glm::vec3 position{ 0.0f, 0.0f, 0.0f };
		glm::vec3 rotationDegrees{ 0.0f, 0.0f, 0.0f };
		glm::vec3 scale{ 1.0f, 1.0f, 1.0f };

		glm::mat4 ToMatrix() const
		{
			glm::mat4 m{ 1.0f };
			m = glm::translate(m, position);
			m = glm::rotate(m, glm::radians(rotationDegrees.z), glm::vec3(0, 0, 1));
			m = glm::rotate(m, glm::radians(rotationDegrees.y), glm::vec3(0, 1, 0));
			m = glm::rotate(m, glm::radians(rotationDegrees.x), glm::vec3(1, 0, 0));
			m = glm::scale(m, scale);
			return m;
		}
	};

	struct Camera
	{
		glm::vec3 position{ 2.2f, 1.6f, 2.2f };
		glm::vec3 target{ 0.0f, 0.0f, 0.0f };
		glm::vec3 up{ 0.0f, 1.0f, 0.0f };

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

		glm::vec3 position{ 0.0f, 0.0f, 0.0f };
		glm::vec3 direction{ 0.0f, -1.0f, 0.0f };

		glm::vec3 color{ 1.0f, 1.0f, 1.0f };
		float intensity{ 1.0f };

		float range{ 10.0f };
		float innerAngleDeg{ 12.0f };
		float outerAngleDeg{ 20.0f };

		float attConstant{ 1.0f };
		float attLinear{ 0.12f };
		float attQuadratic{ 0.04f };
	};

	struct MaterialParams
	{
		glm::vec4 baseColor{ 1.0f, 1.0f, 1.0f, 1.0f };
		float shininess{ 64.0f };
		float specStrength{ 0.5f };
		float shadowBias{ 0.0f }; // reserved for later shadow mapping

		// Cross-backend binding: if non-zero, renderer binds this descriptor at slot t0.
		rhi::TextureDescIndex albedoDescIndex{ 0 };
	};

	enum class MaterialPerm : std::uint32_t
	{
		None      = 0,
		UseTex    = 1u << 0,
		UseShadow = 1u << 1,
		Skinning  = 1u << 2, // later
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
	// Material = "how we draw": parameters + textures + permutation flags.
	// NOTE: UseTex is inferred automatically if albedoDescIndex != 0.
	struct Material
	{
		MaterialParams params{};
		MaterialPerm permFlags{ MaterialPerm::UseShadow };
	};

	inline MaterialPerm EffectivePerm(const Material& m) noexcept
	{
		MaterialPerm p = m.permFlags;
		if (m.params.albedoDescIndex != 0)
		{
			p |= MaterialPerm::UseTex;
		}
		return p;
	}

	struct DrawItem
	{
		// Renderer does NOT own the mesh. Caller is responsible for Upload/Destroy.
		const MeshRHI* mesh{ nullptr };
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

		void Clear()
		{
			drawItems.clear();
			lights.clear();
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
