module;

#include <entt/entt.hpp>
#include <memory>
#include <utility>
#include <vector>

module core:level_ecs_impl;

import :level_ecs;

namespace rendern
{
	using namespace EnTT_helpers;

    struct LevelWorld::Impl
    {
        entt::registry registry{};
    };

    LevelWorld::LevelWorld()
        : impl_(std::make_unique<Impl>())
    {
    }

    LevelWorld::~LevelWorld() = default;
    LevelWorld::LevelWorld(LevelWorld&&) noexcept = default;
    LevelWorld& LevelWorld::operator=(LevelWorld&&) noexcept = default;

    EntityHandle LevelWorld::CreateEntity()
    {
        return FromEnTT(impl_->registry.create());
    }

    void LevelWorld::Clear()
    {
        impl_->registry.clear();
    }

    bool LevelWorld::IsEntityValid(const EntityHandle entity) const
    {
        if (entity == kNullEntity)
        {
            return false;
        }
        return impl_->registry.valid(ToEnTT(entity));
    }

    void LevelWorld::DestroyEntity(const EntityHandle entity)
    {
        if (!IsEntityValid(entity))
        {
            return;
        }

        impl_->registry.destroy(ToEnTT(entity));
    }

    void LevelWorld::EmplaceNodeData(const EntityHandle entity,
        const int nodeIndex,
        const int parentIndex,
        const Transform& local,
        const mathUtils::Mat4& world,
        const Flags& flags)
    {
        const entt::entity e = ToEnTT(entity);
        impl_->registry.emplace<LevelNodeId>(e, nodeIndex);
        impl_->registry.emplace<ParentIndex>(e, parentIndex);
        impl_->registry.emplace<LocalTransform>(e, local);
        impl_->registry.emplace<WorldTransform>(e, world);
        impl_->registry.emplace<Flags>(e, flags);
    }

    void LevelWorld::UpsertNodeData(const EntityHandle entity,
        const int nodeIndex,
        const int parentIndex,
        const Transform& local,
        const mathUtils::Mat4& world,
        const Flags& flags)
    {
        const entt::entity e = ToEnTT(entity);
        impl_->registry.emplace_or_replace<LevelNodeId>(e, nodeIndex);
        impl_->registry.emplace_or_replace<ParentIndex>(e, parentIndex);
        impl_->registry.emplace_or_replace<LocalTransform>(e, local);
        impl_->registry.emplace_or_replace<WorldTransform>(e, world);
        impl_->registry.emplace_or_replace<Flags>(e, flags);
    }

    void LevelWorld::EmplaceRenderable(const EntityHandle entity, const Renderable& renderable)
    {
        impl_->registry.emplace<Renderable>(ToEnTT(entity), renderable);
    }

    void LevelWorld::UpsertRenderable(const EntityHandle entity, const Renderable& renderable)
    {
        impl_->registry.emplace_or_replace<Renderable>(ToEnTT(entity), renderable);
    }

    bool LevelWorld::HasRenderable(const EntityHandle entity) const
    {
        return IsEntityValid(entity) && impl_->registry.all_of<Renderable>(ToEnTT(entity));
    }

    void LevelWorld::RemoveRenderable(const EntityHandle entity)
    {
        if (!IsEntityValid(entity))
        {
            return;
        }

        impl_->registry.remove<Renderable>(ToEnTT(entity));
    }

    std::size_t LevelWorld::GetRenderableCount() const noexcept
    {
        return impl_->registry.storage<Renderable>().size();
    }

    const LevelNodeId* LevelWorld::TryGetLevelNodeIdPtr(const EntityHandle entity) const noexcept
    {
        return IsEntityValid(entity) ? impl_->registry.try_get<LevelNodeId>(ToEnTT(entity)) : nullptr;
    }

    const ParentIndex* LevelWorld::TryGetParentIndexPtr(const EntityHandle entity) const noexcept
    {
        return IsEntityValid(entity) ? impl_->registry.try_get<ParentIndex>(ToEnTT(entity)) : nullptr;
    }

    const LocalTransform* LevelWorld::TryGetLocalTransformPtr(const EntityHandle entity) const noexcept
    {
        return IsEntityValid(entity) ? impl_->registry.try_get<LocalTransform>(ToEnTT(entity)) : nullptr;
    }

    const WorldTransform* LevelWorld::TryGetWorldTransformPtr(const EntityHandle entity) const noexcept
    {
        return IsEntityValid(entity) ? impl_->registry.try_get<WorldTransform>(ToEnTT(entity)) : nullptr;
    }

    const Flags* LevelWorld::TryGetFlagsPtr(const EntityHandle entity) const noexcept
    {
        return IsEntityValid(entity) ? impl_->registry.try_get<Flags>(ToEnTT(entity)) : nullptr;
    }

    const Renderable* LevelWorld::TryGetRenderablePtr(const EntityHandle entity) const noexcept
    {
        return IsEntityValid(entity) ? impl_->registry.try_get<Renderable>(ToEnTT(entity)) : nullptr;
    }

    bool LevelWorld::TryGetLevelNodeId(const EntityHandle entity, LevelNodeId& out) const noexcept
    {
        if (const LevelNodeId* value = TryGetLevelNodeIdPtr(entity))
        {
            out = *value;
            return true;
        }
        return false;
    }

    bool LevelWorld::TryGetParentIndex(const EntityHandle entity, ParentIndex& out) const noexcept
    {
        if (const ParentIndex* value = TryGetParentIndexPtr(entity))
        {
            out = *value;
            return true;
        }
        return false;
    }

    bool LevelWorld::TryGetLocalTransform(const EntityHandle entity, LocalTransform& out) const noexcept
    {
        if (const LocalTransform* value = TryGetLocalTransformPtr(entity))
        {
            out = *value;
            return true;
        }
        return false;
    }

    bool LevelWorld::TryGetWorldTransform(const EntityHandle entity, WorldTransform& out) const noexcept
    {
        if (const WorldTransform* value = TryGetWorldTransformPtr(entity))
        {
            out = *value;
            return true;
        }
        return false;
    }

    bool LevelWorld::TryGetFlags(const EntityHandle entity, Flags& out) const noexcept
    {
        if (const Flags* value = TryGetFlagsPtr(entity))
        {
            out = *value;
            return true;
        }
        return false;
    }

    bool LevelWorld::TryGetRenderable(const EntityHandle entity, Renderable& out) const noexcept
    {
        if (const Renderable* value = TryGetRenderablePtr(entity))
        {
            out = *value;
            return true;
        }
        return false;
    }

    std::vector<EntityHandle> LevelWorld::GatherNodeEntities() const
    {
        std::vector<EntityHandle> result;
        const auto view = impl_->registry.view<LevelNodeId, ParentIndex, LocalTransform, WorldTransform, Flags>();
        result.reserve(impl_->registry.storage<LevelNodeId>().size());

        for (const entt::entity e : view)
        {
            result.push_back(FromEnTT(e));
        }

        return result;
    }

    std::vector<EntityHandle> LevelWorld::GatherRenderableEntities() const
    {
        std::vector<EntityHandle> result;
        const auto view = impl_->registry.view<LevelNodeId, WorldTransform, Renderable, Flags>();
        result.reserve(impl_->registry.storage<Renderable>().size());

        for (const entt::entity e : view)
        {
            result.push_back(FromEnTT(e));
        }

        return result;
    }
}