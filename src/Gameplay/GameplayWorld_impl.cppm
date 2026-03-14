module;

#include <entt/entt.hpp>
#include <utility>

module core:gameplay_impl;

import :gameplay;

namespace rendern
{
    struct GameplayWorld::Impl
    {
        entt::registry registry{};
        std::size_t aliveCount{ 0 };
    };

    GameplayWorld::GameplayWorld()
        : impl_(std::make_unique<Impl>())
    {
    }

    GameplayWorld::~GameplayWorld() = default;

    GameplayWorld::GameplayWorld(GameplayWorld&& other) noexcept = default;
    GameplayWorld& GameplayWorld::operator=(GameplayWorld&& other) noexcept = default;

    EntityHandle GameplayWorld::CreateEntity()
    {
        const entt::entity entity = impl_->registry.create();
        ++impl_->aliveCount;
        return FromEnTT(entity);
    }

    void GameplayWorld::DestroyEntity(const EntityHandle entity)
    {
        if (!IsEntityValid(entity))
        {
            return;
        }

        impl_->registry.destroy(ToEnTT(entity));
        if (impl_->aliveCount > 0)
        {
            --impl_->aliveCount;
        }
    }

    void GameplayWorld::Clear() noexcept
    {
        impl_->registry.clear();
        impl_->aliveCount = 0;
    }

    bool GameplayWorld::IsEntityValid(const EntityHandle entity) const noexcept
    {
        if (entity == kNullEntity)
        {
            return false;
        }

        return impl_->registry.valid(ToEnTT(entity));
    }

    std::size_t GameplayWorld::GetAliveCount() const noexcept
    {
        return impl_->aliveCount;
    }

    void GameplayWorld::AddTransform(const EntityHandle entity, const GameplayTransformComponent& value)
    {
        if (!IsEntityValid(entity))
        {
            return;
        }

        impl_->registry.emplace_or_replace<GameplayTransformComponent>(ToEnTT(entity), value);
    }

    void GameplayWorld::SetTransform(const EntityHandle entity, const GameplayTransformComponent& value)
    {
        AddTransform(entity, value);
    }

    GameplayTransformComponent* GameplayWorld::TryGetTransform(const EntityHandle entity) noexcept
    {
        return IsEntityValid(entity) ? impl_->registry.try_get<GameplayTransformComponent>(ToEnTT(entity)) : nullptr;
    }

    const GameplayTransformComponent* GameplayWorld::TryGetTransform(const EntityHandle entity) const noexcept
    {
        return IsEntityValid(entity) ? impl_->registry.try_get<GameplayTransformComponent>(ToEnTT(entity)) : nullptr;
    }

    bool GameplayWorld::HasTransform(const EntityHandle entity) const noexcept
    {
        return IsEntityValid(entity) && impl_->registry.all_of<GameplayTransformComponent>(ToEnTT(entity));
    }

    void GameplayWorld::RemoveTransform(const EntityHandle entity)
    {
        if (!HasTransform(entity))
        {
            return;
        }

        impl_->registry.remove<GameplayTransformComponent>(ToEnTT(entity));
    }

    void GameplayWorld::AddNodeLink(const EntityHandle entity, const GameplayNodeLinkComponent& value)
    {
        if (!IsEntityValid(entity))
        {
            return;
        }

        impl_->registry.emplace_or_replace<GameplayNodeLinkComponent>(ToEnTT(entity), value);
    }

    void GameplayWorld::SetNodeLink(const EntityHandle entity, const GameplayNodeLinkComponent& value)
    {
        AddNodeLink(entity, value);
    }

    GameplayNodeLinkComponent* GameplayWorld::TryGetNodeLink(const EntityHandle entity) noexcept
    {
        return IsEntityValid(entity) ? impl_->registry.try_get<GameplayNodeLinkComponent>(ToEnTT(entity)) : nullptr;
    }

    const GameplayNodeLinkComponent* GameplayWorld::TryGetNodeLink(const EntityHandle entity) const noexcept
    {
        return IsEntityValid(entity) ? impl_->registry.try_get<GameplayNodeLinkComponent>(ToEnTT(entity)) : nullptr;
    }

    bool GameplayWorld::HasNodeLink(const EntityHandle entity) const noexcept
    {
        return IsEntityValid(entity) && impl_->registry.all_of<GameplayNodeLinkComponent>(ToEnTT(entity));
    }

    void GameplayWorld::RemoveNodeLink(const EntityHandle entity)
    {
        if (!HasNodeLink(entity))
        {
            return;
        }

        impl_->registry.remove<GameplayNodeLinkComponent>(ToEnTT(entity));
    }

    void GameplayWorld::AddAnimationLink(const EntityHandle entity, const GameplayAnimationLinkComponent& value)
    {
        if (!IsEntityValid(entity))
        {
            return;
        }

        impl_->registry.emplace_or_replace<GameplayAnimationLinkComponent>(ToEnTT(entity), value);
    }

    void GameplayWorld::SetAnimationLink(const EntityHandle entity, const GameplayAnimationLinkComponent& value)
    {
        AddAnimationLink(entity, value);
    }

    GameplayAnimationLinkComponent* GameplayWorld::TryGetAnimationLink(const EntityHandle entity) noexcept
    {
        return IsEntityValid(entity) ? impl_->registry.try_get<GameplayAnimationLinkComponent>(ToEnTT(entity)) : nullptr;
    }

    const GameplayAnimationLinkComponent* GameplayWorld::TryGetAnimationLink(const EntityHandle entity) const noexcept
    {
        return IsEntityValid(entity) ? impl_->registry.try_get<GameplayAnimationLinkComponent>(ToEnTT(entity)) : nullptr;
    }

    bool GameplayWorld::HasAnimationLink(const EntityHandle entity) const noexcept
    {
        return IsEntityValid(entity) && impl_->registry.all_of<GameplayAnimationLinkComponent>(ToEnTT(entity));
    }

    void GameplayWorld::RemoveAnimationLink(const EntityHandle entity)
    {
        if (!HasAnimationLink(entity))
        {
            return;
        }

        impl_->registry.remove<GameplayAnimationLinkComponent>(ToEnTT(entity));
    }

    void GameplayWorld::AddPlayerControlled(const EntityHandle entity, const GameplayPlayerControlledComponent& value)
    {
        if (!IsEntityValid(entity))
        {
            return;
        }

        impl_->registry.emplace_or_replace<GameplayPlayerControlledComponent>(ToEnTT(entity), value);
    }

    void GameplayWorld::SetPlayerControlled(const EntityHandle entity, const GameplayPlayerControlledComponent& value)
    {
        AddPlayerControlled(entity, value);
    }

    GameplayPlayerControlledComponent* GameplayWorld::TryGetPlayerControlled(const EntityHandle entity) noexcept
    {
        return IsEntityValid(entity) ? impl_->registry.try_get<GameplayPlayerControlledComponent>(ToEnTT(entity)) : nullptr;
    }

    const GameplayPlayerControlledComponent* GameplayWorld::TryGetPlayerControlled(const EntityHandle entity) const noexcept
    {
        return IsEntityValid(entity) ? impl_->registry.try_get<GameplayPlayerControlledComponent>(ToEnTT(entity)) : nullptr;
    }

    bool GameplayWorld::HasPlayerControlled(const EntityHandle entity) const noexcept
    {
        return IsEntityValid(entity) && impl_->registry.all_of<GameplayPlayerControlledComponent>(ToEnTT(entity));
    }

    void GameplayWorld::RemovePlayerControlled(const EntityHandle entity)
    {
        if (!HasPlayerControlled(entity))
        {
            return;
        }

        impl_->registry.remove<GameplayPlayerControlledComponent>(ToEnTT(entity));
    }

    void GameplayWorld::AddInputIntent(const EntityHandle entity, const GameplayInputIntentComponent& value)
    {
        if (!IsEntityValid(entity))
        {
            return;
        }

        impl_->registry.emplace_or_replace<GameplayInputIntentComponent>(ToEnTT(entity), value);
    }

    void GameplayWorld::SetInputIntent(const EntityHandle entity, const GameplayInputIntentComponent& value)
    {
        AddInputIntent(entity, value);
    }

    GameplayInputIntentComponent* GameplayWorld::TryGetInputIntent(const EntityHandle entity) noexcept
    {
        return IsEntityValid(entity) ? impl_->registry.try_get<GameplayInputIntentComponent>(ToEnTT(entity)) : nullptr;
    }

    const GameplayInputIntentComponent* GameplayWorld::TryGetInputIntent(const EntityHandle entity) const noexcept
    {
        return IsEntityValid(entity) ? impl_->registry.try_get<GameplayInputIntentComponent>(ToEnTT(entity)) : nullptr;
    }

    bool GameplayWorld::HasInputIntent(const EntityHandle entity) const noexcept
    {
        return IsEntityValid(entity) && impl_->registry.all_of<GameplayInputIntentComponent>(ToEnTT(entity));
    }

    void GameplayWorld::RemoveInputIntent(const EntityHandle entity)
    {
        if (!HasInputIntent(entity))
        {
            return;
        }

        impl_->registry.remove<GameplayInputIntentComponent>(ToEnTT(entity));
    }

    void GameplayWorld::AddCharacterCommand(const EntityHandle entity, const GameplayCharacterCommandComponent& value)
    {
        if (!IsEntityValid(entity))
        {
            return;
        }

        impl_->registry.emplace_or_replace<GameplayCharacterCommandComponent>(ToEnTT(entity), value);
    }

    void GameplayWorld::SetCharacterCommand(const EntityHandle entity, const GameplayCharacterCommandComponent& value)
    {
        AddCharacterCommand(entity, value);
    }

    GameplayCharacterCommandComponent* GameplayWorld::TryGetCharacterCommand(const EntityHandle entity) noexcept
    {
        return IsEntityValid(entity) ? impl_->registry.try_get<GameplayCharacterCommandComponent>(ToEnTT(entity)) : nullptr;
    }

    const GameplayCharacterCommandComponent* GameplayWorld::TryGetCharacterCommand(const EntityHandle entity) const noexcept
    {
        return IsEntityValid(entity) ? impl_->registry.try_get<GameplayCharacterCommandComponent>(ToEnTT(entity)) : nullptr;
    }

    bool GameplayWorld::HasCharacterCommand(const EntityHandle entity) const noexcept
    {
        return IsEntityValid(entity) && impl_->registry.all_of<GameplayCharacterCommandComponent>(ToEnTT(entity));
    }

    void GameplayWorld::RemoveCharacterCommand(const EntityHandle entity)
    {
        if (!HasCharacterCommand(entity))
        {
            return;
        }

        impl_->registry.remove<GameplayCharacterCommandComponent>(ToEnTT(entity));
    }

    void GameplayWorld::AddCharacterMotor(const EntityHandle entity, const GameplayCharacterMotorComponent& value)
    {
        if (!IsEntityValid(entity))
        {
            return;
        }

        impl_->registry.emplace_or_replace<GameplayCharacterMotorComponent>(ToEnTT(entity), value);
    }

    void GameplayWorld::SetCharacterMotor(const EntityHandle entity, const GameplayCharacterMotorComponent& value)
    {
        AddCharacterMotor(entity, value);
    }

    GameplayCharacterMotorComponent* GameplayWorld::TryGetCharacterMotor(const EntityHandle entity) noexcept
    {
        return IsEntityValid(entity) ? impl_->registry.try_get<GameplayCharacterMotorComponent>(ToEnTT(entity)) : nullptr;
    }

    const GameplayCharacterMotorComponent* GameplayWorld::TryGetCharacterMotor(const EntityHandle entity) const noexcept
    {
        return IsEntityValid(entity) ? impl_->registry.try_get<GameplayCharacterMotorComponent>(ToEnTT(entity)) : nullptr;
    }

    bool GameplayWorld::HasCharacterMotor(const EntityHandle entity) const noexcept
    {
        return IsEntityValid(entity) && impl_->registry.all_of<GameplayCharacterMotorComponent>(ToEnTT(entity));
    }

    void GameplayWorld::RemoveCharacterMotor(const EntityHandle entity)
    {
        if (!HasCharacterMotor(entity))
        {
            return;
        }

        impl_->registry.remove<GameplayCharacterMotorComponent>(ToEnTT(entity));
    }

    void GameplayWorld::AddCharacterMovementState(const EntityHandle entity, const GameplayCharacterMovementStateComponent& value)
    {
        if (!IsEntityValid(entity))
        {
            return;
        }

        impl_->registry.emplace_or_replace<GameplayCharacterMovementStateComponent>(ToEnTT(entity), value);
    }

    void GameplayWorld::SetCharacterMovementState(const EntityHandle entity, const GameplayCharacterMovementStateComponent& value)
    {
        AddCharacterMovementState(entity, value);
    }

    GameplayCharacterMovementStateComponent* GameplayWorld::TryGetCharacterMovementState(const EntityHandle entity) noexcept
    {
        return IsEntityValid(entity) ? impl_->registry.try_get<GameplayCharacterMovementStateComponent>(ToEnTT(entity)) : nullptr;
    }

    const GameplayCharacterMovementStateComponent* GameplayWorld::TryGetCharacterMovementState(const EntityHandle entity) const noexcept
    {
        return IsEntityValid(entity) ? impl_->registry.try_get<GameplayCharacterMovementStateComponent>(ToEnTT(entity)) : nullptr;
    }

    bool GameplayWorld::HasCharacterMovementState(const EntityHandle entity) const noexcept
    {
        return IsEntityValid(entity) && impl_->registry.all_of<GameplayCharacterMovementStateComponent>(ToEnTT(entity));
    }

    void GameplayWorld::RemoveCharacterMovementState(const EntityHandle entity)
    {
        if (!HasCharacterMovementState(entity))
        {
            return;
        }

        impl_->registry.remove<GameplayCharacterMovementStateComponent>(ToEnTT(entity));
    }

    void GameplayWorld::AddFollowCamera(const EntityHandle entity, const GameplayFollowCameraComponent& value)
    {
        if (!IsEntityValid(entity))
        {
            return;
        }

        impl_->registry.emplace_or_replace<GameplayFollowCameraComponent>(ToEnTT(entity), value);
    }

    void GameplayWorld::SetFollowCamera(const EntityHandle entity, const GameplayFollowCameraComponent& value)
    {
        AddFollowCamera(entity, value);
    }

    GameplayFollowCameraComponent* GameplayWorld::TryGetFollowCamera(const EntityHandle entity) noexcept
    {
        return IsEntityValid(entity) ? impl_->registry.try_get<GameplayFollowCameraComponent>(ToEnTT(entity)) : nullptr;
    }

    const GameplayFollowCameraComponent* GameplayWorld::TryGetFollowCamera(const EntityHandle entity) const noexcept
    {
        return IsEntityValid(entity) ? impl_->registry.try_get<GameplayFollowCameraComponent>(ToEnTT(entity)) : nullptr;
    }

    bool GameplayWorld::HasFollowCamera(const EntityHandle entity) const noexcept
    {
        return IsEntityValid(entity) && impl_->registry.all_of<GameplayFollowCameraComponent>(ToEnTT(entity));
    }

    void GameplayWorld::RemoveFollowCamera(const EntityHandle entity)
    {
        if (!HasFollowCamera(entity))
        {
            return;
        }

        impl_->registry.remove<GameplayFollowCameraComponent>(ToEnTT(entity));
    }

    void GameplayWorld::AddLocomotion(const EntityHandle entity, const GameplayLocomotionComponent& value)
    {
        if (!IsEntityValid(entity))
        {
            return;
        }

        impl_->registry.emplace_or_replace<GameplayLocomotionComponent>(ToEnTT(entity), value);
    }

    void GameplayWorld::SetLocomotion(const EntityHandle entity, const GameplayLocomotionComponent& value)
    {
        AddLocomotion(entity, value);
    }

    GameplayLocomotionComponent* GameplayWorld::TryGetLocomotion(const EntityHandle entity) noexcept
    {
        return IsEntityValid(entity) ? impl_->registry.try_get<GameplayLocomotionComponent>(ToEnTT(entity)) : nullptr;
    }

    const GameplayLocomotionComponent* GameplayWorld::TryGetLocomotion(const EntityHandle entity) const noexcept
    {
        return IsEntityValid(entity) ? impl_->registry.try_get<GameplayLocomotionComponent>(ToEnTT(entity)) : nullptr;
    }

    bool GameplayWorld::HasLocomotion(const EntityHandle entity) const noexcept
    {
        return IsEntityValid(entity) && impl_->registry.all_of<GameplayLocomotionComponent>(ToEnTT(entity));
    }

    void GameplayWorld::RemoveLocomotion(const EntityHandle entity)
    {
        if (!HasLocomotion(entity))
        {
            return;
        }

        impl_->registry.remove<GameplayLocomotionComponent>(ToEnTT(entity));
    }

    void GameplayWorld::AddAction(const EntityHandle entity, const GameplayActionComponent& value)
    {
        if (!IsEntityValid(entity))
        {
            return;
        }

        impl_->registry.emplace_or_replace<GameplayActionComponent>(ToEnTT(entity), value);
    }

    void GameplayWorld::SetAction(const EntityHandle entity, const GameplayActionComponent& value)
    {
        AddAction(entity, value);
    }

    GameplayActionComponent* GameplayWorld::TryGetAction(const EntityHandle entity) noexcept
    {
        return IsEntityValid(entity) ? impl_->registry.try_get<GameplayActionComponent>(ToEnTT(entity)) : nullptr;
    }

    const GameplayActionComponent* GameplayWorld::TryGetAction(const EntityHandle entity) const noexcept
    {
        return IsEntityValid(entity) ? impl_->registry.try_get<GameplayActionComponent>(ToEnTT(entity)) : nullptr;
    }

    bool GameplayWorld::HasAction(const EntityHandle entity) const noexcept
    {
        return IsEntityValid(entity) && impl_->registry.all_of<GameplayActionComponent>(ToEnTT(entity));
    }

    void GameplayWorld::RemoveAction(const EntityHandle entity)
    {
        if (!HasAction(entity))
        {
            return;
        }

        impl_->registry.remove<GameplayActionComponent>(ToEnTT(entity));
    }

    void GameplayWorld::AddAnimationNotifyState(const EntityHandle entity, const GameplayAnimationNotifyStateComponent& value)
    {
        if (!IsEntityValid(entity))
        {
            return;
        }

        impl_->registry.emplace_or_replace<GameplayAnimationNotifyStateComponent>(ToEnTT(entity), value);
    }

    void GameplayWorld::SetAnimationNotifyState(const EntityHandle entity, const GameplayAnimationNotifyStateComponent& value)
    {
        AddAnimationNotifyState(entity, value);
    }

    GameplayAnimationNotifyStateComponent* GameplayWorld::TryGetAnimationNotifyState(const EntityHandle entity) noexcept
    {
        return IsEntityValid(entity) ? impl_->registry.try_get<GameplayAnimationNotifyStateComponent>(ToEnTT(entity)) : nullptr;
    }

    const GameplayAnimationNotifyStateComponent* GameplayWorld::TryGetAnimationNotifyState(const EntityHandle entity) const noexcept
    {
        return IsEntityValid(entity) ? impl_->registry.try_get<GameplayAnimationNotifyStateComponent>(ToEnTT(entity)) : nullptr;
    }

    bool GameplayWorld::HasAnimationNotifyState(const EntityHandle entity) const noexcept
    {
        return IsEntityValid(entity) && impl_->registry.all_of<GameplayAnimationNotifyStateComponent>(ToEnTT(entity));
    }

    void GameplayWorld::RemoveAnimationNotifyState(const EntityHandle entity)
    {
        if (!HasAnimationNotifyState(entity))
        {
            return;
        }

        impl_->registry.remove<GameplayAnimationNotifyStateComponent>(ToEnTT(entity));
    }
}
