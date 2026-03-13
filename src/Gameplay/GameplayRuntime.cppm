module;

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

export module core:gameplay_runtime;

import :gameplay;
import :input;
import :level;
import :scene;
import :animation_controller;
import :math_utils;

export namespace rendern
{
    struct GameplayAnimationNotifyRecord
    {
        EntityHandle entity{ kNullEntity };
        int nodeIndex{ -1 };
        int skinnedDrawIndex{ -1 };
        AnimationNotifyEvent event{};
    };

    struct GameplayUpdateContext
    {
        float deltaSeconds{ 0.0f };
        const InputState* input{ nullptr };
        LevelAsset* levelAsset{ nullptr };
        LevelInstance* levelInstance{ nullptr };
        Scene* scene{ nullptr };
    };

    class GameplayRuntime
    {
    public:
        GameplayRuntime() = default;

        void Initialize(LevelAsset& levelAsset, LevelInstance& levelInstance, Scene& scene)
        {
            GameplayUpdateContext ctx{};
            ctx.levelAsset = &levelAsset;
            ctx.levelInstance = &levelInstance;
            ctx.scene = &scene;
            EnsureBootstrapEntity_(ctx);
        }

        void Shutdown()
        {
            world_.Clear();
            recentNotifyEvents_.clear();
            controlledEntity_ = kNullEntity;
        }

        void BeginFrame()
        {
            recentNotifyEvents_.clear();

            if (!world_.IsEntityValid(controlledEntity_))
            {
                return;
            }

            if (GameplayInputIntentComponent* intent = world_.TryGetInputIntent(controlledEntity_))
            {
                *intent = {};
            }

            if (GameplayActionComponent* action = world_.TryGetAction(controlledEntity_))
            {
                action->requested = GameplayActionKind::None;
            }
        }

        void PreAnimationUpdate(const GameplayUpdateContext& ctx)
        {
            EnsureBootstrapEntity_(ctx);
            UpdatePrimaryPlayerIntent_(ctx);
        }

        void PostAnimationUpdate(const GameplayUpdateContext& ctx)
        {
            if (!world_.IsEntityValid(controlledEntity_) || ctx.levelInstance == nullptr || ctx.scene == nullptr)
            {
                return;
            }

            const GameplayNodeLinkComponent* nodeLink = world_.TryGetNodeLink(controlledEntity_);
            GameplayAnimationLinkComponent* animLink = world_.TryGetAnimationLink(controlledEntity_);
            if (nodeLink == nullptr || animLink == nullptr || animLink->skinnedDrawIndex < 0)
            {
                return;
            }

            if (SkinnedDrawItem* skinnedItem = ctx.levelInstance->GetSkinnedDrawItem(*ctx.scene, animLink->skinnedDrawIndex))
            {
                std::vector<AnimationNotifyEvent> events = ConsumeAnimationControllerNotifyEvents(skinnedItem->controller);
                for (AnimationNotifyEvent& event : events)
                {
                    recentNotifyEvents_.push_back(GameplayAnimationNotifyRecord{
                        .entity = controlledEntity_,
                        .nodeIndex = nodeLink->nodeIndex,
                        .skinnedDrawIndex = animLink->skinnedDrawIndex,
                        .event = std::move(event)
                        });
                }
            }
        }

        [[nodiscard]] GameplayWorld& GetWorld() noexcept
        {
            return world_;
        }

        [[nodiscard]] const GameplayWorld& GetWorld() const noexcept
        {
            return world_;
        }

        [[nodiscard]] EntityHandle GetControlledEntity() const noexcept
        {
            return controlledEntity_;
        }

        [[nodiscard]] const std::vector<GameplayAnimationNotifyRecord>& GetRecentNotifyEvents() const noexcept
        {
            return recentNotifyEvents_;
        }

        [[nodiscard]] EntityHandle SpawnNodeBoundEntity(const GameplayUpdateContext& ctx, const int nodeIndex, const bool playerControlled = false)
        {
            if (ctx.levelAsset == nullptr || ctx.levelInstance == nullptr || ctx.scene == nullptr)
            {
                return kNullEntity;
            }
            if (nodeIndex < 0 || static_cast<std::size_t>(nodeIndex) >= ctx.levelAsset->nodes.size())
            {
                return kNullEntity;
            }

            const EntityHandle entity = world_.CreateEntity();
            if (!BindEntityToNode(ctx, entity, nodeIndex))
            {
                world_.DestroyEntity(entity);
                return kNullEntity;
            }

            world_.AddInputIntent(entity);
            world_.AddCharacterMotor(entity);
            world_.AddLocomotion(entity);
            world_.AddAction(entity);
            if (playerControlled)
            {
                world_.AddPlayerControlled(entity);
                controlledEntity_ = entity;
            }
            return entity;
        }

        [[nodiscard]] bool BindEntityToNode(const GameplayUpdateContext& ctx, const EntityHandle entity, const int nodeIndex)
        {
            if (!world_.IsEntityValid(entity) || ctx.levelAsset == nullptr || ctx.levelInstance == nullptr)
            {
                return false;
            }
            if (nodeIndex < 0 || static_cast<std::size_t>(nodeIndex) >= ctx.levelAsset->nodes.size())
            {
                return false;
            }

            const LevelNode& node = ctx.levelAsset->nodes[static_cast<std::size_t>(nodeIndex)];
            if (!node.alive)
            {
                return false;
            }

            world_.AddTransform(entity,
                GameplayTransformComponent{
                    .position = node.transform.position,
                    .rotationDegrees = node.transform.rotationDegrees,
                    .scale = node.transform.scale
                });

            world_.AddNodeLink(entity,
                GameplayNodeLinkComponent{
                    .nodeIndex = nodeIndex,
                    .levelEntity = ctx.levelInstance->GetNodeEntity(nodeIndex)
                });

            const int skinnedDrawIndex = ctx.levelInstance->GetNodeSkinnedDrawIndex(nodeIndex);
            if (skinnedDrawIndex >= 0)
            {
                world_.AddAnimationLink(entity,
                    GameplayAnimationLinkComponent{
                        .skinnedDrawIndex = skinnedDrawIndex,
                        .controllerAssetId = node.animationController
                    });
            }
            else
            {
                world_.RemoveAnimationLink(entity);
            }

            return true;
        }

    private:
        static constexpr int kVkLButton = 0x01;
        static constexpr int kVkSpace = 0x20;
        static constexpr int kVkShift = 0x10;

        [[nodiscard]] static float NormalizeMoveAxis_(const float x, const float y, float& outX, float& outY) noexcept
        {
            const float lenSq = x * x + y * y;
            if (lenSq <= 1e-8f)
            {
                outX = 0.0f;
                outY = 0.0f;
                return 0.0f;
            }

            const float len = std::sqrt(lenSq);
            outX = x / len;
            outY = y / len;
            return len;
        }

        void UpdatePrimaryPlayerIntent_(const GameplayUpdateContext& ctx)
        {
            if (controlledEntity_ == kNullEntity || ctx.input == nullptr || !world_.IsEntityValid(controlledEntity_))
            {
                return;
            }

            GameplayInputIntentComponent* intent = world_.TryGetInputIntent(controlledEntity_);
            GameplayLocomotionComponent* locomotion = world_.TryGetLocomotion(controlledEntity_);
            if (intent == nullptr)
            {
                return;
            }

            const InputState& input = *ctx.input;
            const float rawX =
                (input.KeyDown('D') ? 1.0f : 0.0f) -
                (input.KeyDown('A') ? 1.0f : 0.0f);
            const float rawY =
                (input.KeyDown('W') ? 1.0f : 0.0f) -
                (input.KeyDown('S') ? 1.0f : 0.0f);

            float moveX = 0.0f;
            float moveY = 0.0f;
            const float moveLen = NormalizeMoveAxis_(rawX, rawY, moveX, moveY);

            intent->moveX = moveX;
            intent->moveY = moveY;
            intent->runHeld = input.KeyDown(kVkShift) || input.shiftDown;
            intent->jumpPressed = input.KeyPressed(kVkSpace);
            intent->attackPressed = input.KeyPressed(kVkLButton);
            intent->interactPressed = input.KeyPressed('E');

            if (GameplayCharacterMotorComponent* motor = world_.TryGetCharacterMotor(controlledEntity_))
            {
                motor->desiredMoveWorld = mathUtils::Vec3(moveX, 0.0f, moveY);
            }

            if (locomotion != nullptr)
            {
                locomotion->planarSpeed = moveLen;
                locomotion->isMoving = moveLen > 1e-4f;
                locomotion->isRunning = locomotion->isMoving && intent->runHeld;
            }

            if (GameplayActionComponent* action = world_.TryGetAction(controlledEntity_))
            {
                if (intent->attackPressed)
                {
                    action->requested = GameplayActionKind::LightAttack;
                }
                else if (intent->interactPressed)
                {
                    action->requested = GameplayActionKind::Interact;
                }
            }
        }

        void EnsureBootstrapEntity_(const GameplayUpdateContext& ctx)
        {
            if (controlledEntity_ != kNullEntity && world_.IsEntityValid(controlledEntity_))
            {
                return;
            }
            if (ctx.levelAsset == nullptr || ctx.levelInstance == nullptr || ctx.scene == nullptr)
            {
                return;
            }

            const int bootstrapNodeIndex = FindBootstrapNodeIndex_(*ctx.levelAsset, *ctx.levelInstance);
            if (bootstrapNodeIndex < 0)
            {
                return;
            }

            SpawnNodeBoundEntity(ctx, bootstrapNodeIndex, true);
        }

        [[nodiscard]] static int FindBootstrapNodeIndex_(const LevelAsset& levelAsset, const LevelInstance& levelInstance) noexcept
        {
            for (std::size_t i = 0; i < levelAsset.nodes.size(); ++i)
            {
                const LevelNode& node = levelAsset.nodes[i];
                if (!node.alive || !node.visible)
                {
                    continue;
                }
                if (levelInstance.GetNodeSkinnedDrawIndex(static_cast<int>(i)) >= 0)
                {
                    return static_cast<int>(i);
                }
            }

            for (std::size_t i = 0; i < levelAsset.nodes.size(); ++i)
            {
                const LevelNode& node = levelAsset.nodes[i];
                if (node.alive && node.visible)
                {
                    return static_cast<int>(i);
                }
            }
            return -1;
        }

    private:
        GameplayWorld world_{};
        EntityHandle controlledEntity_{ kNullEntity };
        std::vector<GameplayAnimationNotifyRecord> recentNotifyEvents_{};
    };
}