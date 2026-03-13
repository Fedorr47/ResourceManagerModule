module;

#include <cstdint>
#include <limits>
#include <memory>
#include <vector>
#include <cstddef>
#include <concepts>
#include <functional>
#include <utility>

export module core:level_ecs;

import :scene;
import :math_utils;
import :EnTTHelpers;

export namespace rendern
{
	using namespace EnTT_helpers;

    struct LevelNodeId
    {
        int index{ -1 };
    };

    struct ParentIndex
    {
        int parent{ -1 };
    };

    struct LocalTransform
    {
        Transform local{};
    };

    struct WorldTransform
    {
        mathUtils::Mat4 world{ 1.0f };
    };

    struct Renderable
    {
        MeshHandle mesh{};
        MaterialHandle material{};
        int drawIndex{ -1 };
        int skinnedDrawIndex{ -1 };
        bool isSkinned{ false };
    };

    struct Flags
    {
        bool alive{ true };
        bool visible{ true };
    };

    template <class Fn>
    concept LevelNodeVisitor =
        std::invocable<Fn&, EntityHandle,
        const LevelNodeId&,
        const ParentIndex&,
        const LocalTransform&,
        const WorldTransform&,
        const Flags&>;

    template <class Fn>
    concept RenderableVisitor =
        std::invocable<Fn&, EntityHandle,
        const LevelNodeId&,
        const WorldTransform&,
        const Renderable&,
        const Flags&>;

    class LevelWorld
    {
    public:
        LevelWorld();
        ~LevelWorld();
        LevelWorld(LevelWorld&&) noexcept;
        LevelWorld& operator=(LevelWorld&&) noexcept;

        LevelWorld(const LevelWorld&) = delete;
        LevelWorld& operator=(const LevelWorld&) = delete;

        EntityHandle CreateEntity();
        void Clear();

        bool IsEntityValid(EntityHandle entity) const;
        void DestroyEntity(EntityHandle entity);

        void EmplaceNodeData(EntityHandle entity,
            int nodeIndex,
            int parentIndex,
            const Transform& local,
            const mathUtils::Mat4& world,
            const Flags& flags);

        void UpsertNodeData(EntityHandle entity,
            int nodeIndex,
            int parentIndex,
            const Transform& local,
            const mathUtils::Mat4& world,
            const Flags& flags);

        void EmplaceRenderable(EntityHandle entity, const Renderable& renderable);
        void UpsertRenderable(EntityHandle entity, const Renderable& renderable);
        bool HasRenderable(EntityHandle entity) const;
        void RemoveRenderable(EntityHandle entity);

        std::size_t GetRenderableCount() const noexcept;

        const LevelNodeId* TryGetLevelNodeIdPtr(EntityHandle entity) const noexcept;
        const ParentIndex* TryGetParentIndexPtr(EntityHandle entity) const noexcept;
        const LocalTransform* TryGetLocalTransformPtr(EntityHandle entity) const noexcept;
        const WorldTransform* TryGetWorldTransformPtr(EntityHandle entity) const noexcept;
        const Flags* TryGetFlagsPtr(EntityHandle entity) const noexcept;
        const Renderable* TryGetRenderablePtr(EntityHandle entity) const noexcept;

        bool TryGetLevelNodeId(EntityHandle entity, LevelNodeId& out) const noexcept;
        bool TryGetParentIndex(EntityHandle entity, ParentIndex& out) const noexcept;
        bool TryGetLocalTransform(EntityHandle entity, LocalTransform& out) const noexcept;
        bool TryGetWorldTransform(EntityHandle entity, WorldTransform& out) const noexcept;
        bool TryGetFlags(EntityHandle entity, Flags& out) const noexcept;
        bool TryGetRenderable(EntityHandle entity, Renderable& out) const noexcept;

        template <class Fn>
            requires LevelNodeVisitor<Fn>
        void ForEachNode(Fn&& fn) const
        {
            const std::vector<EntityHandle> entities = GatherNodeEntities();
            for (const EntityHandle entity : entities)
            {
                const LevelNodeId* nodeId = TryGetLevelNodeIdPtr(entity);
                const ParentIndex* parent = TryGetParentIndexPtr(entity);
                const LocalTransform* local = TryGetLocalTransformPtr(entity);
                const WorldTransform* world = TryGetWorldTransformPtr(entity);
                const Flags* flags = TryGetFlagsPtr(entity);

                if (!nodeId || !parent || !local || !world || !flags)
                {
                    continue;
                }

                std::invoke(std::forward<Fn>(fn), entity, *nodeId, *parent, *local, *world, *flags);
            }
        }

        template <class Fn>
            requires RenderableVisitor<Fn>
        void ForEachRenderable(Fn&& fn) const
        {
            const std::vector<EntityHandle> entities = GatherRenderableEntities();
            for (const EntityHandle entity : entities)
            {
                const LevelNodeId* nodeId = TryGetLevelNodeIdPtr(entity);
                const WorldTransform* world = TryGetWorldTransformPtr(entity);
                const Renderable* renderable = TryGetRenderablePtr(entity);
                const Flags* flags = TryGetFlagsPtr(entity);

                if (!nodeId || !world || !renderable || !flags)
                {
                    continue;
                }

                std::invoke(std::forward<Fn>(fn), entity, *nodeId, *world, *renderable, *flags);
            }
        }

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_{};

        std::vector<EntityHandle> GatherNodeEntities() const;
        std::vector<EntityHandle> GatherRenderableEntities() const;
    };
}