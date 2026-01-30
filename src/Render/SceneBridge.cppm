module;

#include <cstdint>
#include <vector>
#include <span>

export module core:scene_bridge;

import :rhi;
import :scene;

export namespace rendern
{
	struct float4x4
	{
		float mat[16]{};
	};

	struct CameraData
	{
		float4x4 viewProj;
	};

	struct MeshHandle
	{
		std::uint32_t id{};
	};

	struct MaterialHandle
	{
		std::uint32_t id{};
	};

	class RenderWorld
	{
	public:
		void Clear()
		{
			drawItems_.clear();
		}
		void Add(DrawItem item)
		{
			drawItems_.push_back(item);
		}
		std::span<const DrawItem> GetDrawItems() const
		{
			return drawItems_;
		}
	private:
		std::vector<DrawItem> drawItems_;
	};

	// Optional interface for extracting render data from a game world/scene.
	// engine-side scene module/part can implement this and feed RenderWorld.
	class ISceneProvider
	{
	public:
		virtual ~ISceneProvider() = default;

		virtual void Extract(RenderWorld& outWorld) const = 0;
		virtual CameraData GetMainCamera() const = 0;
	};
}