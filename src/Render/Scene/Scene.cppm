module;

#include <cstdint>
#include <vector>
#include <span>
#include <stdexcept>
#include <memory>
#include <algorithm>
#include <cmath>

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

	struct Particle
	{
		mathUtils::Vec3 position{ 0.0f, 0.0f, 0.0f };
		mathUtils::Vec3 velocity{ 0.0f, 0.0f, 0.0f };
		mathUtils::Vec4 color{ 1.0f, 0.6f, 0.2f, 1.0f };
		mathUtils::Vec4 colorBegin{ 1.0f, 0.6f, 0.2f, 1.0f };
		mathUtils::Vec4 colorEnd{ 1.0f, 0.6f, 0.2f, 1.0f };
		float size{ 0.15f };
		float sizeBegin{ 0.15f };
		float sizeEnd{ 0.15f };
		float lifetime{ 1.0f };
		float age{ 0.0f };
		float rotationRad{ 0.0f };
		bool alive{ true };
		int ownerEmitter{ -1 };
	};

	struct ParticleEmitter
	{
		std::string name;
		std::string textureId;
		rhi::TextureDescIndex textureDescIndex{ 0 };
		bool enabled{ true };
		bool looping{ true };
		mathUtils::Vec3 position{ 0.0f, 0.0f, 0.0f };
		mathUtils::Vec3 positionJitter{ 0.0f, 0.0f, 0.0f };
		mathUtils::Vec3 velocityMin{ -0.15f, 0.9f, -0.15f };
		mathUtils::Vec3 velocityMax{ 0.15f, 1.6f, 0.15f };
		mathUtils::Vec4 color{ 1.0f, 0.6f, 0.2f, 1.0f };
		mathUtils::Vec4 colorBegin{ 1.0f, 0.6f, 0.2f, 1.0f };
		mathUtils::Vec4 colorEnd{ 1.0f, 0.6f, 0.2f, 1.0f };
		float sizeMin{ 0.08f };
		float sizeMax{ 0.16f };
		float sizeBegin{ 0.0f };
		float sizeEnd{ 0.0f };
		float lifetimeMin{ 0.8f };
		float lifetimeMax{ 1.4f };
		float spawnRate{ 16.0f };
		std::uint32_t burstCount{ 0u };
		float duration{ 0.0f };
		float startDelay{ 0.0f };
		std::uint32_t maxParticles{ 1024u };

		// Runtime-only state. Not serialized.
		float elapsed{ 0.0f };
		float spawnAccumulator{ 0.0f };
		std::uint32_t spawnSequence{ 0u };
		bool burstDone{ false };
	};

	namespace detail
	{
		inline std::uint32_t NextParticleRand(std::uint32_t& state) noexcept
		{
			state = state * 1664525u + 1013904223u;
			return state;
		}

		inline float ParticleRand01(std::uint32_t& state) noexcept
		{
			return static_cast<float>(NextParticleRand(state) & 0x00FFFFFFu) / 16777215.0f;
		}

		inline float ParticleRandRange(std::uint32_t& state, float lo, float hi) noexcept
		{
			return lo + (hi - lo) * ParticleRand01(state);
		}
	}

	enum class GizmoAxis : std::uint8_t
	{
		None = 0,
		X,
		Y,
		Z,
		XY,
		XZ,
		YZ,
		XYZ
	};

	enum class GizmoMode : std::uint8_t
	{
		None = 0,
		Translate,
		Rotate,
		Scale
	};

	enum class GizmoSpace : std::uint8_t
	{
		World = 0,
		Local
	};

	struct TranslateGizmoState
	{
		bool enabled{ true };
		bool visible{ false };
		GizmoAxis hoveredAxis{ GizmoAxis::None };
		GizmoAxis activeAxis{ GizmoAxis::None };
		mathUtils::Vec3 pivotWorld{ 0.0f, 0.0f, 0.0f };
		float axisLengthWorld{ 1.0f };
		mathUtils::Vec3 axisXWorld{ 1.0f, 0.0f, 0.0f };
		mathUtils::Vec3 axisYWorld{ 0.0f, 1.0f, 0.0f };
		mathUtils::Vec3 axisZWorld{ 0.0f, 0.0f, 1.0f };
	};

	struct ParticleEmitterTranslateDragState
	{
		bool dragging{ false };
		GizmoAxis activeAxis{ GizmoAxis::None };
		mathUtils::Vec3 dragStartEmitterPosition{ 0.0f, 0.0f, 0.0f };
		mathUtils::Vec3 dragStartWorldHit{ 0.0f, 0.0f, 0.0f };
		mathUtils::Vec3 dragPlaneNormal{ 0.0f, 1.0f, 0.0f };
		mathUtils::Vec3 dragAxisWorld{ 1.0f, 0.0f, 0.0f };
	};

	struct RotateGizmoState
	{
		bool enabled{ true };
		bool visible{ false };
		GizmoAxis hoveredAxis{ GizmoAxis::None };
		GizmoAxis activeAxis{ GizmoAxis::None };
		mathUtils::Vec3 pivotWorld{ 0.0f, 0.0f, 0.0f };
		mathUtils::Vec3 axisXWorld{ 1.0f, 0.0f, 0.0f };
		mathUtils::Vec3 axisYWorld{ 0.0f, 1.0f, 0.0f };
		mathUtils::Vec3 axisZWorld{ 0.0f, 0.0f, 1.0f };
		float ringRadiusWorld{ 1.0f };
	};

	struct ScaleGizmoState
	{
		bool enabled{ true };
		bool visible{ false };
		GizmoAxis hoveredAxis{ GizmoAxis::None };
		GizmoAxis activeAxis{ GizmoAxis::None };
		mathUtils::Vec3 pivotWorld{ 0.0f, 0.0f, 0.0f };
		mathUtils::Vec3 axisXWorld{ 1.0f, 0.0f, 0.0f };
		mathUtils::Vec3 axisYWorld{ 0.0f, 1.0f, 0.0f };
		mathUtils::Vec3 axisZWorld{ 0.0f, 0.0f, 1.0f };
		float axisLengthWorld{ 1.0f };
		float uniformHandleRadiusWorld{ 0.12f };
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
		rhi::TextureDescIndex specularDescIndex{ 0 };
		rhi::TextureDescIndex glossDescIndex{ 0 };
	};

	enum class EnvSource : std::uint8_t
	{
		Skybox = 0,
		ReflectionCapture = 1
	};

	enum class MaterialPerm : std::uint32_t
	{
		None = 0,
		UseTex = 1u << 0,
		UseShadow = 1u << 1,
		Skinning = 1u << 2,
		Transparent = 1u << 3,
		PlanarMirror = 1u << 4
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
		EnvSource envSource{ EnvSource::Skybox };
	};

	inline MaterialPerm EffectivePerm(const Material& material) noexcept
	{
		MaterialPerm materialPerm = material.permFlags;
		// a < 1 => transparent even if flag isn't set.
		if (material.params.baseColor.w < 0.999f)
		{
			materialPerm |= MaterialPerm::Transparent;
		}
		if (material.params.albedoDescIndex != 0)
		{
			materialPerm |= MaterialPerm::UseTex;
		}
		return materialPerm;
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
		std::vector<Particle> particles;
		std::vector<ParticleEmitter> particleEmitters;

		rhi::TextureDescIndex skyboxDescIndex{ 0 };

		DebugRay debugPickRay{};

		// Editor selection (runtime-only). Index into LevelAsset::nodes.
		int editorSelectedNode{ -1 };

		// Editor selection (runtime-only). Index into LevelAsset::particleEmitters.
		int editorSelectedParticleEmitter{ -1 };

		// Multi-selection (runtime-only). Indices into LevelAsset::nodes.
		// Convention:
		//  - editorSelectedNodes holds the full set (no duplicates).
		//  - editorSelectedNode is the "primary" selection (usually last clicked).
		std::vector<int> editorSelectedNodes;

		// Editor selection (runtime-only). Index into Scene::drawItems (or -1).
		int editorSelectedDrawItem{ -1 };

		// Multi-selection (runtime-only). Indices into Scene::drawItems.
		std::vector<int> editorSelectedDrawItems;

		// Reflection capture owner (runtime-only).
		// Node index is stable (LevelAsset::nodes). DrawItem index is derived from LevelInstance mapping.
		int editorReflectionCaptureOwnerNode{ -1 };
		int editorReflectionCaptureOwnerDrawItem{ -1 };

		// Active editor gizmo mode (runtime-only).
		GizmoMode editorGizmoMode{ GizmoMode::Translate };
		GizmoSpace editorTranslateSpace{ GizmoSpace::World };

		// Editor translate gizmo (runtime-only).
		TranslateGizmoState editorTranslateGizmo{};

		// Editor rotate gizmo (runtime-only).
		RotateGizmoState editorRotateGizmo{};

		// Editor translate drag state for particle emitters.
		ParticleEmitterTranslateDragState editorParticleEmitterTranslateDrag{};

		// Editor scale gizmo (runtime-only).
		ScaleGizmoState editorScaleGizmo{};

		void Clear()
		{
			drawItems.clear();
			lights.clear();
			particles.clear();
			particleEmitters.clear();
			skyboxDescIndex = 0;
			debugPickRay = {};
			editorSelectedNode = -1;
			editorSelectedParticleEmitter = -1;
			editorSelectedNodes.clear();
			editorSelectedDrawItem = -1;
			editorSelectedDrawItems.clear();
			editorReflectionCaptureOwnerNode = -1;
			editorReflectionCaptureOwnerDrawItem = -1;
			editorGizmoMode = GizmoMode::Translate;
			editorTranslateSpace = GizmoSpace::World;
			editorTranslateGizmo = {};
			editorRotateGizmo = {};
			editorParticleEmitterTranslateDrag = {};
			editorScaleGizmo = {};
		}

		void EditorClearSelection() noexcept
		{
			editorSelectedNode = -1;
			editorSelectedParticleEmitter = -1;
			editorSelectedNodes.clear();
			editorSelectedDrawItem = -1;
			editorSelectedDrawItems.clear();
		}

		bool EditorIsNodeSelected(int nodeIndex) const noexcept
		{
			for (const int v : editorSelectedNodes)
			{
				if (v == nodeIndex)
				{
					return true;
				}
			}
			return false;
		}

		// Sets selection to a single node (or clears when nodeIndex < 0).
		void EditorSetSelectionSingle(int nodeIndex) noexcept
		{
			EditorClearSelection();
			if (nodeIndex >= 0)
			{
				editorSelectedNode = nodeIndex;
				editorSelectedNodes.push_back(nodeIndex);
			}
		}

		bool EditorIsParticleEmitterSelected(int emitterIndex) const noexcept
		{
			return editorSelectedParticleEmitter == emitterIndex;
		}

		void EditorSetSelectionSingleParticleEmitter(int emitterIndex) noexcept
		{
			EditorClearSelection();
			if (emitterIndex >= 0)
			{
				editorSelectedParticleEmitter = emitterIndex;
			}
		}


		void EditorToggleSelectionParticleEmitter(int emitterIndex) noexcept
		{
			if (emitterIndex < 0)
			{
				return;
			}

			if (editorSelectedParticleEmitter == emitterIndex)
			{
				editorSelectedParticleEmitter = -1;
			}
			else
			{
				EditorClearSelection();
				editorSelectedParticleEmitter = emitterIndex;
			}
		}

		// Toggle node in the selection set. Updates primary selection.
		void EditorToggleSelectionNode(int nodeIndex) noexcept
		{
			if (nodeIndex < 0)
			{
				return;
			}

			for (std::size_t i = 0; i < editorSelectedNodes.size(); ++i)
			{
				if (editorSelectedNodes[i] == nodeIndex)
				{
					editorSelectedNodes.erase(editorSelectedNodes.begin() + static_cast<std::vector<int>::difference_type>(i));
					if (editorSelectedNode == nodeIndex)
					{
						editorSelectedNode = editorSelectedNodes.empty() ? -1 : editorSelectedNodes.back();
					}
					return;
				}
			}

			editorSelectedNodes.push_back(nodeIndex);
			editorSelectedNode = nodeIndex;
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

		Particle& AddParticle(const Particle& particle)
		{
			particles.push_back(particle);
			return particles.back();
		}

		ParticleEmitter& AddParticleEmitter(const ParticleEmitter& emitter)
		{
			ParticleEmitter runtime = emitter;
			runtime.elapsed = 0.0f;
			runtime.spawnAccumulator = 0.0f;
			runtime.spawnSequence = 0u;
			runtime.burstDone = false;
			particleEmitters.push_back(runtime);
			return particleEmitters.back();
		}

		void EmitParticleFromEmitter(ParticleEmitter& emitter, int emitterIndex)
		{
			if (emitter.maxParticles > 0u)
			{
				std::uint32_t aliveOwned = 0u;
				for (const Particle& existing : particles)
				{
					if (existing.alive && existing.ownerEmitter == emitterIndex)
					{
						++aliveOwned;
					}
				}
				if (aliveOwned >= emitter.maxParticles)
				{
					return;
				}
			}

			std::uint32_t rng = (emitter.spawnSequence++ + 1u) * 747796405u + 2891336453u;

			Particle particle{};
			particle.position =
				emitter.position +
				mathUtils::Vec3(
					detail::ParticleRandRange(rng, -emitter.positionJitter.x, emitter.positionJitter.x),
					detail::ParticleRandRange(rng, -emitter.positionJitter.y, emitter.positionJitter.y),
					detail::ParticleRandRange(rng, -emitter.positionJitter.z, emitter.positionJitter.z));

			particle.velocity = mathUtils::Vec3(
				detail::ParticleRandRange(rng, emitter.velocityMin.x, emitter.velocityMax.x),
				detail::ParticleRandRange(rng, emitter.velocityMin.y, emitter.velocityMax.y),
				detail::ParticleRandRange(rng, emitter.velocityMin.z, emitter.velocityMax.z));

			particle.colorBegin = emitter.colorBegin;
			particle.colorEnd = emitter.colorEnd;
			particle.color = particle.colorBegin;
			const float randomizedSizeBegin = detail::ParticleRandRange(rng, emitter.sizeMin, emitter.sizeMax);
			particle.sizeBegin = (emitter.sizeBegin > 0.0f) ? emitter.sizeBegin : randomizedSizeBegin;
			particle.sizeEnd = (emitter.sizeEnd > 0.0f) ? emitter.sizeEnd : particle.sizeBegin;
			particle.size = particle.sizeBegin;
			particle.lifetime = detail::ParticleRandRange(rng, emitter.lifetimeMin, emitter.lifetimeMax);
			particle.age = 0.0f;
			particle.rotationRad = detail::ParticleRandRange(rng, 0.0f, 6.28318530718f);
			particle.alive = true;
			particle.ownerEmitter = emitterIndex;

			particles.push_back(particle);
		}

		void UpdateParticles(float dt)
		{
			for (std::size_t emitterIndex = 0; emitterIndex < particleEmitters.size(); ++emitterIndex)
			{
				ParticleEmitter& emitter = particleEmitters[emitterIndex];
				if (!emitter.enabled)
				{
					continue;
				}

				const float previousElapsed = emitter.elapsed;
				emitter.elapsed += dt;

				if (!emitter.burstDone && emitter.burstCount > 0u && previousElapsed <= emitter.startDelay && emitter.elapsed >= emitter.startDelay)
				{
					for (std::uint32_t i = 0; i < emitter.burstCount; ++i)
					{
						EmitParticleFromEmitter(emitter, static_cast<int>(emitterIndex));
					}
					emitter.burstDone = true;
				}

				if (emitter.elapsed < emitter.startDelay)
				{
					continue;
				}

				if (!emitter.looping && emitter.duration > 0.0f && (emitter.elapsed - emitter.startDelay) > emitter.duration)
				{
					continue;
				}

				if (emitter.spawnRate > 0.0f)
				{
					emitter.spawnAccumulator += dt * emitter.spawnRate;
					while (emitter.spawnAccumulator >= 1.0f)
					{
						emitter.spawnAccumulator -= 1.0f;
						EmitParticleFromEmitter(emitter, static_cast<int>(emitterIndex));
					}
				}
			}

			for (Particle& particle : particles)
			{
				if (!particle.alive)
				{
					continue;
				}

				particle.age += dt;
				particle.position = particle.position + particle.velocity * dt;
				const float lifeT = (particle.lifetime > 0.0f) ? std::clamp(particle.age / particle.lifetime, 0.0f, 1.0f) : 1.0f;
				particle.color = mathUtils::Lerp(particle.colorBegin, particle.colorEnd, lifeT);
				particle.size = mathUtils::Lerp(particle.sizeBegin, particle.sizeEnd, lifeT);

				if (particle.lifetime > 0.0f && particle.age >= particle.lifetime)
				{
					particle.alive = false;
				}
			}

			particles.erase(
				std::remove_if(particles.begin(), particles.end(), [](const Particle& particle)
					{
						return !particle.alive || particle.size <= 0.0f || particle.color.w <= 0.0f;
					}),
				particles.end());
		}

		std::span<const Material> GetMaterials() const { return materials; }
		std::span<Material> GetMaterials() { return materials; }

		std::span<const DrawItem> GetDrawItems() const { return drawItems; }
		std::span<DrawItem> GetDrawItems() { return drawItems; }

		std::span<const Light> GetLights() const { return lights; }
		std::span<Light> GetLights() { return lights; }

		std::span<const Particle> GetParticles() const { return particles; }
		std::span<Particle> GetParticles() { return particles; }

		void RestartParticleEmitter(int emitterIndex)
		{
			if (emitterIndex < 0 || static_cast<std::size_t>(emitterIndex) >= particleEmitters.size())
			{
				return;
			}

			particles.erase(
				std::remove_if(particles.begin(), particles.end(), [emitterIndex](const Particle& particle)
					{
						return particle.ownerEmitter == emitterIndex;
					}),
				particles.end());

			ParticleEmitter& emitter = particleEmitters[static_cast<std::size_t>(emitterIndex)];
			emitter.elapsed = 0.0f;
			emitter.spawnAccumulator = 0.0f;
			emitter.spawnSequence = 0u;
			emitter.burstDone = false;
		}

		void RestartAllParticleEmitters()
		{
			particles.clear();
			for (ParticleEmitter& emitter : particleEmitters)
			{
				emitter.elapsed = 0.0f;
				emitter.spawnAccumulator = 0.0f;
				emitter.spawnSequence = 0u;
				emitter.burstDone = false;
			}
		}
		std::span<const ParticleEmitter> GetParticleEmitters() const { return particleEmitters; }
		std::span<ParticleEmitter> GetParticleEmitters() { return particleEmitters; }
	};
} // namespace rendern