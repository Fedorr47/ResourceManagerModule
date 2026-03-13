module;

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>

export module core:gameplay;

import :math_utils;
import :scene;
import :level_ecs;
import :EnTTHelpers;

export namespace rendern
{
    using namespace EnTT_helpers;

    struct GameplayTransformComponent
    {
        mathUtils::Vec3 position{ 0.0f, 0.0f, 0.0f };
        mathUtils::Vec3 rotationDegrees{ 0.0f, 0.0f, 0.0f };
        mathUtils::Vec3 scale{ 1.0f, 1.0f, 1.0f };
    };

    struct GameplayNodeLinkComponent
    {
        int nodeIndex{ -1 };
        EntityHandle levelEntity{ kNullEntity };
    };

    struct GameplayAnimationLinkComponent
    {
        int skinnedDrawIndex{ -1 };
        std::string controllerAssetId{};
    };

    struct GameplayPlayerControlledComponent
    {
        bool isPrimary{ true };
    };

    struct GameplayInputIntentComponent
    {
        float moveX{ 0.0f };
        float moveY{ 0.0f };
        bool runHeld{ false };
        bool jumpPressed{ false };
        bool attackPressed{ false };
        bool interactPressed{ false };
    };

    struct GameplayCharacterMotorComponent
    {
        mathUtils::Vec3 velocity{ 0.0f, 0.0f, 0.0f };
        mathUtils::Vec3 desiredMoveWorld{ 0.0f, 0.0f, 0.0f };
        float maxWalkSpeed{ 2.0f };
        float maxRunSpeed{ 4.5f };
        float acceleration{ 12.0f };
        float deceleration{ 16.0f };
    };

    struct GameplayLocomotionComponent
    {
        float planarSpeed{ 0.0f };
        bool isMoving{ false };
        bool isRunning{ false };
    };

    enum class GameplayActionKind : std::uint8_t
    {
        None = 0,
        LightAttack,
        Interact
    };

    struct GameplayActionComponent
    {
        GameplayActionKind current{ GameplayActionKind::None };
        GameplayActionKind requested{ GameplayActionKind::None };
        bool busy{ false };
    };

    class GameplayWorld
    {
    public:
        GameplayWorld();
        ~GameplayWorld();

        GameplayWorld(GameplayWorld&& other) noexcept;
        GameplayWorld& operator=(GameplayWorld&& other) noexcept;

        GameplayWorld(const GameplayWorld&) = delete;
        GameplayWorld& operator=(const GameplayWorld&) = delete;

        [[nodiscard]] EntityHandle CreateEntity();
        void DestroyEntity(EntityHandle entity);
        void Clear() noexcept;
        [[nodiscard]] bool IsEntityValid(EntityHandle entity) const noexcept;
        [[nodiscard]] std::size_t GetAliveCount() const noexcept;

        void AddTransform(EntityHandle entity, const GameplayTransformComponent& value);
        void SetTransform(EntityHandle entity, const GameplayTransformComponent& value);
        [[nodiscard]] GameplayTransformComponent* TryGetTransform(EntityHandle entity) noexcept;
        [[nodiscard]] const GameplayTransformComponent* TryGetTransform(EntityHandle entity) const noexcept;
        [[nodiscard]] bool HasTransform(EntityHandle entity) const noexcept;
        void RemoveTransform(EntityHandle entity);

        void AddNodeLink(EntityHandle entity, const GameplayNodeLinkComponent& value);
        void SetNodeLink(EntityHandle entity, const GameplayNodeLinkComponent& value);
        [[nodiscard]] GameplayNodeLinkComponent* TryGetNodeLink(EntityHandle entity) noexcept;
        [[nodiscard]] const GameplayNodeLinkComponent* TryGetNodeLink(EntityHandle entity) const noexcept;
        [[nodiscard]] bool HasNodeLink(EntityHandle entity) const noexcept;
        void RemoveNodeLink(EntityHandle entity);

        void AddAnimationLink(EntityHandle entity, const GameplayAnimationLinkComponent& value);
        void SetAnimationLink(EntityHandle entity, const GameplayAnimationLinkComponent& value);
        [[nodiscard]] GameplayAnimationLinkComponent* TryGetAnimationLink(EntityHandle entity) noexcept;
        [[nodiscard]] const GameplayAnimationLinkComponent* TryGetAnimationLink(EntityHandle entity) const noexcept;
        [[nodiscard]] bool HasAnimationLink(EntityHandle entity) const noexcept;
        void RemoveAnimationLink(EntityHandle entity);

        void AddPlayerControlled(EntityHandle entity, const GameplayPlayerControlledComponent& value = {});
        void SetPlayerControlled(EntityHandle entity, const GameplayPlayerControlledComponent& value);
        [[nodiscard]] GameplayPlayerControlledComponent* TryGetPlayerControlled(EntityHandle entity) noexcept;
        [[nodiscard]] const GameplayPlayerControlledComponent* TryGetPlayerControlled(EntityHandle entity) const noexcept;
        [[nodiscard]] bool HasPlayerControlled(EntityHandle entity) const noexcept;
        void RemovePlayerControlled(EntityHandle entity);

        void AddInputIntent(EntityHandle entity, const GameplayInputIntentComponent& value = {});
        void SetInputIntent(EntityHandle entity, const GameplayInputIntentComponent& value);
        [[nodiscard]] GameplayInputIntentComponent* TryGetInputIntent(EntityHandle entity) noexcept;
        [[nodiscard]] const GameplayInputIntentComponent* TryGetInputIntent(EntityHandle entity) const noexcept;
        [[nodiscard]] bool HasInputIntent(EntityHandle entity) const noexcept;
        void RemoveInputIntent(EntityHandle entity);

        void AddCharacterMotor(EntityHandle entity, const GameplayCharacterMotorComponent& value = {});
        void SetCharacterMotor(EntityHandle entity, const GameplayCharacterMotorComponent& value);
        [[nodiscard]] GameplayCharacterMotorComponent* TryGetCharacterMotor(EntityHandle entity) noexcept;
        [[nodiscard]] const GameplayCharacterMotorComponent* TryGetCharacterMotor(EntityHandle entity) const noexcept;
        [[nodiscard]] bool HasCharacterMotor(EntityHandle entity) const noexcept;
        void RemoveCharacterMotor(EntityHandle entity);

        void AddLocomotion(EntityHandle entity, const GameplayLocomotionComponent& value = {});
        void SetLocomotion(EntityHandle entity, const GameplayLocomotionComponent& value);
        [[nodiscard]] GameplayLocomotionComponent* TryGetLocomotion(EntityHandle entity) noexcept;
        [[nodiscard]] const GameplayLocomotionComponent* TryGetLocomotion(EntityHandle entity) const noexcept;
        [[nodiscard]] bool HasLocomotion(EntityHandle entity) const noexcept;
        void RemoveLocomotion(EntityHandle entity);

        void AddAction(EntityHandle entity, const GameplayActionComponent& value = {});
        void SetAction(EntityHandle entity, const GameplayActionComponent& value);
        [[nodiscard]] GameplayActionComponent* TryGetAction(EntityHandle entity) noexcept;
        [[nodiscard]] const GameplayActionComponent* TryGetAction(EntityHandle entity) const noexcept;
        [[nodiscard]] bool HasAction(EntityHandle entity) const noexcept;
        void RemoveAction(EntityHandle entity);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_{};
    };
}