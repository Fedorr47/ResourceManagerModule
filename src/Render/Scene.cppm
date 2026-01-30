module;

#include <cstdint>
#include <vector>
#include <span>

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

	struct DrawItem
	{
		// Renderer does NOT own the mesh. Caller is responsible for Upload/Destroy.
		const MeshRHI* mesh{ nullptr };
		Transform transform{};
		MaterialParams material{};
	};

	class Scene
	{
	public:
		Camera camera{};
		std::vector<DrawItem> drawItems;
		std::vector<Light> lights;

		void Clear()
		{
			drawItems.clear();
			lights.clear();
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

		std::span<const DrawItem> GetDrawItems() const { return drawItems; }
		std::span<DrawItem> GetDrawItems() { return drawItems; }

		std::span<const Light> GetLights() const { return lights; }
		std::span<Light> GetLights() { return lights; }
	};
} // namespace rendern
