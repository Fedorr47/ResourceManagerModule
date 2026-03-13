module;

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <string>
#include <utility>
#include <string_view>
#include <functional>
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

    struct GameplayAxisKeyBinding
    {
        int negativeKey{ 0 };
        int positiveKey{ 0 };
    };

    struct GameplayButtonKeyBinding
    {
        int key{ 0 };
    };

    struct GameplayKeyboardMouseBindings
    {
        GameplayAxisKeyBinding moveX{ 'L', 'J' };
        GameplayAxisKeyBinding moveY{ 'K', 'I' };
        GameplayButtonKeyBinding run{ 0x10 };
        GameplayButtonKeyBinding jump{ 0x20 };
        GameplayButtonKeyBinding attack{ 0x01 };
        GameplayButtonKeyBinding interact{ 'E' };
    };

    using GameplayIntentSourceCallback = std::function<void(
        EntityHandle entity,
        const GameplayUpdateContext& ctx,
        GameplayWorld& world,
        GameplayInputIntentComponent& outIntent,
        GameplayActionComponent* action)>;

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
            intentBindings_.clear();
            motorEntities_.clear();
            nodeBoundEntities_.clear();
            controlledEntity_ = kNullEntity;
        }

        void BindIntentSource(const EntityHandle entity, GameplayIntentSourceCallback callback)
        {
            recentNotifyEvents_.clear();

            if (!world_.IsEntityValid(entity) || !callback)
            {
                return;
            }

            UpsertIntentBinding_(entity, std::move(callback));
        }

        void BindKeyboardMouseIntentSource(const EntityHandle entity, const GameplayKeyboardMouseBindings& bindings = {})
        {
            BindIntentSource(entity,
                [bindings]([[maybe_unused]] const EntityHandle entity,
                    const GameplayUpdateContext& ctx,
                    [[maybe_unused]] GameplayWorld& world,
                    GameplayInputIntentComponent& outIntent,
                    GameplayActionComponent* action)
                {
                    if (ctx.input == nullptr)
                    {
                        return;
                    }

                    const InputState& input = *ctx.input;
                    float moveX = 0.0f;
                    float moveY = 0.0f;
                    GameplayRuntime::ReadAxisFromKeys_(input, bindings.moveX, moveX);
                    GameplayRuntime::ReadAxisFromKeys_(input, bindings.moveY, moveY);
                    GameplayRuntime::NormalizeMoveAxis_(moveX, moveY, outIntent.moveX, outIntent.moveY);

                    outIntent.runHeld = GameplayRuntime::ReadHeldButton_(input, bindings.run);
                    outIntent.jumpPressed = GameplayRuntime::ReadPressedButton_(input, bindings.jump);
                    outIntent.attackPressed = GameplayRuntime::ReadPressedButton_(input, bindings.attack);
                    outIntent.interactPressed = GameplayRuntime::ReadPressedButton_(input, bindings.interact);

                    if (action != nullptr)
                    {
                        if (outIntent.jumpPressed)
                        {
                            GameplayRuntime::QueueActionRequest_(*action, GameplayActionKind::Jump);
                        }
                        else if (outIntent.attackPressed)
                        {
                            GameplayRuntime::QueueActionRequest_(*action, GameplayActionKind::LightAttack);
                        }
                        else if (outIntent.interactPressed)
                        {
                            GameplayRuntime::QueueActionRequest_(*action, GameplayActionKind::Interact);
                        }
                    }
                });
        }

        void UnbindIntentSource(const EntityHandle entity)
        {
            intentBindings_.erase(
                std::remove_if(intentBindings_.begin(),
                    intentBindings_.end(),
                    [entity](const IntentBinding& binding)
                    {
                        return binding.entity == entity;
                    }),
                intentBindings_.end());
        }

        void BeginFrame()
        {
            recentNotifyEvents_.clear();
            CompactTrackedState_();

            for (const EntityHandle entity : motorEntities_)
            {
                if (GameplayInputIntentComponent* intent = world_.TryGetInputIntent(entity))
                {
                    *intent = {};
                }

                if (GameplayLocomotionComponent* locomotion = world_.TryGetLocomotion(entity))
                {
                    locomotion->moveX = 0.0f;
                    locomotion->moveY = 0.0f;
                }
            }

            for (const EntityHandle entity : nodeBoundEntities_)
            {
                if (GameplayAnimationNotifyStateComponent* notifyState = world_.TryGetAnimationNotifyState(entity))
                {
                    ResetAnimationNotifyStateFrame_(*notifyState);
                }
            }
        }

        void PreAnimationUpdate(const GameplayUpdateContext& ctx)
        {
            EnsureBootstrapEntity_(ctx);
            UpdateIntentSources_(ctx);
            UpdateMotorEntities_(ctx);
            SyncGameplayTransformsToRuntime_(ctx);
            SyncGameplayLocomotionToAnimation_(ctx);
            SyncGameplayActionsToAnimation_(ctx);
        }

        void PostAnimationUpdate(const GameplayUpdateContext& ctx)
        {
            if (ctx.levelInstance == nullptr || ctx.scene == nullptr)
            {
                return;
            }

            for (const EntityHandle entity : nodeBoundEntities_)
            {
                const GameplayNodeLinkComponent* nodeLink = world_.TryGetNodeLink(entity);
                GameplayAnimationLinkComponent* animLink = world_.TryGetAnimationLink(entity);
                GameplayAnimationNotifyStateComponent* notifyState = world_.TryGetAnimationNotifyState(entity);
                if (nodeLink == nullptr || animLink == nullptr || notifyState == nullptr || animLink->skinnedDrawIndex < 0)
                {
                    continue;
                }

                SkinnedDrawItem* skinnedItem = ctx.levelInstance->GetSkinnedDrawItem(*ctx.scene, animLink->skinnedDrawIndex);
                if (skinnedItem == nullptr)
                {
                    continue;
                }

                std::vector<AnimationNotifyEvent> events = ConsumeAnimationControllerNotifyEvents(skinnedItem->controller);
                if (events.empty())
                {
                    continue;
                }

                GameplayActionComponent* action = world_.TryGetAction(entity);

                for (AnimationNotifyEvent& event : events)
                {
                    recentNotifyEvents_.push_back(GameplayAnimationNotifyRecord{
                        .entity = entity,
                        .nodeIndex = nodeLink->nodeIndex,
                        .skinnedDrawIndex = animLink->skinnedDrawIndex,
                        .event = event
                        });

                    ApplyAnimationNotifyToGameplay_(*notifyState, action, event);
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
            world_.AddAnimationNotifyState(entity);

            TrackMotorEntity_(entity);
            TrackNodeBoundEntity_(entity);

            if (playerControlled)
            {
                world_.AddPlayerControlled(entity);
                BindKeyboardMouseIntentSource(entity);
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

            world_.AddAnimationNotifyState(entity);

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
        struct IntentBinding
        {
            EntityHandle entity{ kNullEntity };
            GameplayIntentSourceCallback callback{};
        };

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

        [[nodiscard]] static bool ReadHeldButton_(const InputState& input, const GameplayButtonKeyBinding& binding) noexcept
        {
            return binding.key != 0 && input.KeyDown(binding.key);
        }

        [[nodiscard]] static bool ReadPressedButton_(const InputState& input, const GameplayButtonKeyBinding& binding) noexcept
        {
            return binding.key != 0 && input.KeyPressed(binding.key);
        }

        static void ReadAxisFromKeys_(const InputState& input, const GameplayAxisKeyBinding& binding, float& outValue) noexcept
        {
            const float positive = (binding.positiveKey != 0 && input.KeyDown(binding.positiveKey)) ? 1.0f : 0.0f;
            const float negative = (binding.negativeKey != 0 && input.KeyDown(binding.negativeKey)) ? 1.0f : 0.0f;
            outValue = positive - negative;
        }

        static void CompactEntityVector_(std::vector<EntityHandle>& entities, const GameplayWorld& world)
        {
            entities.erase(
                std::remove_if(entities.begin(),
                    entities.end(),
                    [&world](const EntityHandle entity)
                    {
                        return entity == kNullEntity || !world.IsEntityValid(entity);
                    }),
                entities.end());
        }

        void CompactTrackedState_()
        {
            CompactEntityVector_(motorEntities_, world_);
            CompactEntityVector_(nodeBoundEntities_, world_);

            intentBindings_.erase(
                std::remove_if(intentBindings_.begin(),
                    intentBindings_.end(),
                    [this](const IntentBinding& binding)
                    {
                        return binding.entity == kNullEntity ||
                            !binding.callback ||
                            !world_.IsEntityValid(binding.entity);
                    }),
                intentBindings_.end());

            if (controlledEntity_ != kNullEntity && !world_.IsEntityValid(controlledEntity_))
            {
                controlledEntity_ = kNullEntity;
            }
        }

        void UpsertIntentBinding_(const EntityHandle entity, GameplayIntentSourceCallback callback)
        {
            for (IntentBinding& binding : intentBindings_)
            {
                if (binding.entity == entity)
                {
                    binding.callback = std::move(callback);
                    return;
                }
            }

            intentBindings_.push_back(IntentBinding{
                .entity = entity,
                .callback = std::move(callback)
                });
        }

        void TrackMotorEntity_(const EntityHandle entity)
        {
            if (entity == kNullEntity)
            {
                return;
            }

            for (const EntityHandle tracked : motorEntities_)
            {
                if (tracked == entity)
                {
                    return;
                }
            }
            motorEntities_.push_back(entity);
        }

        void TrackNodeBoundEntity_(const EntityHandle entity)
        {
            if (entity == kNullEntity)
            {
                return;
            }

            for (const EntityHandle tracked : nodeBoundEntities_)
            {
                if (tracked == entity)
                {
                    return;
                }
            }
            nodeBoundEntities_.push_back(entity);
        }

        static void BuildPlanarMovementBasis_(const Camera& camera, mathUtils::Vec3& outRight, mathUtils::Vec3& outForward) noexcept
        {
            mathUtils::Vec3 forward = camera.target - camera.position;
            forward.y = 0.0f;
            if (mathUtils::Length(forward) <= 1e-6f)
            {
                forward = mathUtils::Vec3(0.0f, 0.0f, 1.0f);
            }
            else
            {
                forward = mathUtils::Normalize(forward);
            }

            const mathUtils::Vec3 worldUp(0.0f, 1.0f, 0.0f);
            mathUtils::Vec3 right = mathUtils::Cross(worldUp, forward);
            if (mathUtils::Length(right) <= 1e-6f)
            {
                right = mathUtils::Vec3(1.0f, 0.0f, 0.0f);
            }
            else
            {
                right = mathUtils::Normalize(right);
            }

            outRight = right;
            outForward = forward;
        }

        [[nodiscard]] static std::string CanonicalizeAnimationParameterName_(std::string_view name)
        {
            std::string canonical;
            canonical.reserve(name.size());
            for (const unsigned char ch : name)
            {
                if (std::isalnum(ch))
                {
                    canonical.push_back(static_cast<char>(std::tolower(ch)));
                }
            }
            return canonical;
        }

        [[nodiscard]] static bool AnimationParameterNameMatchesAnyAlias_(
            const std::string_view candidate,
            const std::initializer_list<std::string_view> aliases)
        {
            const std::string canonicalCandidate = CanonicalizeAnimationParameterName_(candidate);
            for (const std::string_view alias : aliases)
            {
                if (canonicalCandidate == CanonicalizeAnimationParameterName_(alias))
                {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] static const AnimationParameterDesc* FindAnimationParameterByAliases_(
            const AnimationControllerAsset& asset,
            const AnimationParameterType expectedType,
            const std::initializer_list<std::string_view> aliases)
        {
            for (const AnimationParameterDesc& desc : asset.parameters)
            {
                if (desc.defaultValue.type != expectedType)
                {
                    continue;
                }
                if (AnimationParameterNameMatchesAnyAlias_(desc.name, aliases))
                {
                    return &desc;
                }
            }
            return nullptr;
        }

        static void SetAnimationFloatParameterByAliases_(
            AnimationControllerRuntime& controller,
            const std::initializer_list<std::string_view> aliases,
            const float value)
        {
            if (controller.stateMachineAsset == nullptr)
            {
                return;
            }

            if (const AnimationParameterDesc* desc = FindAnimationParameterByAliases_(
                *controller.stateMachineAsset,
                AnimationParameterType::Float,
                aliases))
            {
                SetAnimationParameter(controller.parameters, desc->name, value);
            }
        }

        static void SetAnimationBoolParameterByAliases_(
            AnimationControllerRuntime& controller,
            const std::initializer_list<std::string_view> aliases,
            const bool value)
        {
            if (controller.stateMachineAsset == nullptr)
            {
                return;
            }

            if (const AnimationParameterDesc* desc = FindAnimationParameterByAliases_(
                *controller.stateMachineAsset,
                AnimationParameterType::Bool,
                aliases))
            {
                SetAnimationParameter(controller.parameters, desc->name, value);
            }
        }

        static void SetAnimationIntParameterByAliases_(
            AnimationControllerRuntime& controller,
            const std::initializer_list<std::string_view> aliases,
            const int value)
        {
            if (controller.stateMachineAsset == nullptr)
            {
                return;
            }

            if (const AnimationParameterDesc* desc = FindAnimationParameterByAliases_(
                *controller.stateMachineAsset,
                AnimationParameterType::Int,
                aliases))
            {
                SetAnimationParameter(controller.parameters, desc->name, value);
            }
        }

        static void SetAnimationNumericParameterByAliases_(
            AnimationControllerRuntime& controller,
            const std::initializer_list<std::string_view> aliases,
            const int value)
        {
            SetAnimationIntParameterByAliases_(controller, aliases, value);
            SetAnimationFloatParameterByAliases_(controller, aliases, static_cast<float>(value));
        }

        static void FireAnimationTriggerByAliases_(
            AnimationControllerRuntime& controller,
            const std::initializer_list<std::string_view> aliases)
        {
            if (controller.stateMachineAsset == nullptr)
            {
                return;
            }

            if (const AnimationParameterDesc* desc = FindAnimationParameterByAliases_(
                *controller.stateMachineAsset,
                AnimationParameterType::Trigger,
                aliases))
            {
                FireAnimationTrigger(controller.parameters, desc->name);
            }
        }

        [[nodiscard]] static GameplayActionKind InferGameplayActionKindFromNotifyId_(const std::string_view notifyId) noexcept
        {
            if (NotifyIdMatchesAnyAlias_(
                notifyId,
                { "LightAttackBegin", "LightAttackStart", "AttackBegin", "AttackStart", "Attack" }))
            {
                return GameplayActionKind::LightAttack;
            }

            if (NotifyIdMatchesAnyAlias_(
                notifyId,
                { "InteractBegin", "InteractStart", "UseBegin", "UseStart", "Use" }))
            {
                return GameplayActionKind::Interact;
            }

            if (NotifyIdMatchesAnyAlias_(
                notifyId,
                { "JumpBegin", "JumpStart", "Jump" }))
            {
                return GameplayActionKind::Jump;
            }

            return GameplayActionKind::None;
        }

        static void QueueActionRequest_(GameplayActionComponent& action, const GameplayActionKind kind) noexcept
        {
            if (kind == GameplayActionKind::None || action.busy)
            {
                return;
            }

            if (action.requested != GameplayActionKind::None)
            {
                return;
            }

            action.requested = kind;
            action.requestDispatched = false;
        }

        [[nodiscard]] static bool NotifyIdMatchesAnyAlias_(
            const std::string_view candidate,
            const std::initializer_list<std::string_view> aliases)
        {
            return AnimationParameterNameMatchesAnyAlias_(candidate, aliases);
        }

        static void ResetAnimationNotifyStateFrame_(GameplayAnimationNotifyStateComponent& notifyState)
        {
            notifyState.anyThisFrame = false;
            notifyState.footstepThisFrame = false;
            notifyState.interactionPointThisFrame = false;
            notifyState.actionStartedThisFrame = false;
            notifyState.actionFinishedThisFrame = false;
            notifyState.hitWindowOpenedThisFrame = false;
            notifyState.hitWindowClosedThisFrame = false;
        }

        static void ApplyAnimationNotifyToGameplay_(
            GameplayAnimationNotifyStateComponent& notifyState,
            GameplayActionComponent* action,
            const AnimationNotifyEvent& event)
        {
            notifyState.anyThisFrame = true;
            notifyState.lastSequence = event.sequence;
            notifyState.lastNormalizedTime = event.normalizedTime;
            notifyState.lastNotifyId = event.id;
            notifyState.lastStateName = event.stateName;
            notifyState.lastClipName = event.clipName;

            if (NotifyIdMatchesAnyAlias_(event.id, { "Footstep", "Step", "FootStep" }))
            {
                notifyState.footstepThisFrame = true;
            }

            if (NotifyIdMatchesAnyAlias_(event.id, { "Interact", "Interaction", "InteractionPoint", "UsePoint" }))
            {
                notifyState.interactionPointThisFrame = true;
            }

            if (NotifyIdMatchesAnyAlias_(
                event.id,
                { "HitWindowBegin", "HitWindowStart", "AttackWindowBegin", "AttackWindowStart", "HitEnable", "EnableHit", "DamageWindowBegin", "DamageOn" }))
            {
                notifyState.hitWindowOpenedThisFrame = true;
                notifyState.hitWindowActive = true;
            }

            if (NotifyIdMatchesAnyAlias_(
                event.id,
                { "HitWindowEnd", "HitWindowStop", "AttackWindowEnd", "AttackWindowStop", "HitDisable", "DisableHit", "DamageWindowEnd", "DamageOff" }))
            {
                notifyState.hitWindowClosedThisFrame = true;
                notifyState.hitWindowActive = false;
            }

            if (NotifyIdMatchesAnyAlias_(
                event.id,
                { "ActionBegin", "ActionStart", "AttackBegin", "AttackStart", "LightAttackBegin", "LightAttackStart", "InteractBegin", "InteractStart", "JumpBegin", "JumpStart" }))
            {
                notifyState.actionStartedThisFrame = true;
                if (action != nullptr)
                {
                    action->busy = true;
                    GameplayActionKind startedKind = action->requested;
                    if (startedKind == GameplayActionKind::None)
                    {
                        startedKind = InferGameplayActionKindFromNotifyId_(event.id);
                    }
                    if (startedKind != GameplayActionKind::None)
                    {
                        action->current = startedKind;
                    }

                    action->requested = GameplayActionKind::None;
                    action->requestDispatched = false;
                }
            }

            if (NotifyIdMatchesAnyAlias_(
                event.id,
                { "ActionEnd", "ActionFinish", "ActionFinished", "AttackEnd", "AttackFinish", "AttackFinished", "LightAttackEnd", "InteractEnd", "InteractFinish", "JumpEnd", "JumpFinish" }))
            {
                notifyState.actionFinishedThisFrame = true;
                if (action != nullptr)
                {
                    action->busy = false;
                    action->current = GameplayActionKind::None;
                    action->requested = GameplayActionKind::None;
                    action->requestDispatched = false;
                }
            }
        }

        void UpdateIntentSources_(const GameplayUpdateContext& ctx)
        {
            for (IntentBinding& binding : intentBindings_)
            {
                if (!world_.IsEntityValid(binding.entity) || !binding.callback)
                {
                    continue;
                }

                GameplayInputIntentComponent* intent = world_.TryGetInputIntent(binding.entity);
                if (intent == nullptr)
                {
                    continue;
                }

                GameplayActionComponent* action = world_.TryGetAction(binding.entity);
                binding.callback(binding.entity, ctx, world_, *intent, action);

                float normalizedX = 0.0f;
                float normalizedY = 0.0f;
                NormalizeMoveAxis_(intent->moveX, intent->moveY, normalizedX, normalizedY);
                intent->moveX = normalizedX;
                intent->moveY = normalizedY;
            }
        }

        void UpdateMotorEntities_(const GameplayUpdateContext& ctx)
        {
            if (ctx.scene == nullptr)
            {
                return;
            }

            mathUtils::Vec3 basisRight(1.0f, 0.0f, 0.0f);
            mathUtils::Vec3 basisForward(0.0f, 0.0f, 1.0f);
            BuildPlanarMovementBasis_(ctx.scene->camera, basisRight, basisForward);

            for (const EntityHandle entity : motorEntities_)
            {
                GameplayTransformComponent* transform = world_.TryGetTransform(entity);
                GameplayInputIntentComponent* intent = world_.TryGetInputIntent(entity);
                GameplayCharacterMotorComponent* motor = world_.TryGetCharacterMotor(entity);
                GameplayLocomotionComponent* locomotion = world_.TryGetLocomotion(entity);
                if (transform == nullptr || intent == nullptr || motor == nullptr)
                {
                    continue;
                }

                mathUtils::Vec3 desiredMoveWorld =
                    basisRight * intent->moveX +
                    basisForward * intent->moveY;

                if (mathUtils::Length(desiredMoveWorld) > 1e-6f)
                {
                    desiredMoveWorld = mathUtils::Normalize(desiredMoveWorld);
                }
                else
                {
                    desiredMoveWorld = mathUtils::Vec3(0.0f, 0.0f, 0.0f);
                }

                motor->desiredMoveWorld = desiredMoveWorld;

                const float targetSpeed = intent->runHeld ? motor->maxRunSpeed : motor->maxWalkSpeed;
                const mathUtils::Vec3 targetVelocity = desiredMoveWorld * targetSpeed;
                const mathUtils::Vec3 velocityDelta = targetVelocity - motor->velocity;

                const float currentSpeed = mathUtils::Length(motor->velocity);
                const float desiredSpeed = mathUtils::Length(targetVelocity);
                const float rate = desiredSpeed > currentSpeed ? motor->acceleration : motor->deceleration;
                const float maxDelta = std::max(rate, 0.0f) * std::max(ctx.deltaSeconds, 0.0f);
                const float deltaLen = mathUtils::Length(velocityDelta);

                if (deltaLen <= maxDelta || maxDelta <= 1e-6f)
                {
                    motor->velocity = targetVelocity;
                }
                else
                {
                    motor->velocity = motor->velocity + (velocityDelta * (maxDelta / deltaLen));
                }

                transform->position = transform->position + motor->velocity * ctx.deltaSeconds;

                const float planarSpeed = mathUtils::Length(motor->velocity);
                const bool isMoving = planarSpeed > 1e-4f;
                if (isMoving)
                {
                    transform->rotationDegrees.y = mathUtils::RadToDeg(std::atan2(motor->velocity.x, motor->velocity.z));
                }

                if (locomotion != nullptr)
                {
                    locomotion->moveX = intent->moveX;
                    locomotion->moveY = intent->moveY;
                    locomotion->planarSpeed = planarSpeed;
                    locomotion->isMoving = isMoving;
                    locomotion->isRunning = isMoving && intent->runHeld;
                }

            }
        }

        void SyncGameplayLocomotionToAnimation_(const GameplayUpdateContext& ctx)
        {
            if (ctx.levelInstance == nullptr || ctx.scene == nullptr)
            {
                return;
            }

            for (const EntityHandle entity : nodeBoundEntities_)
            {
                const GameplayLocomotionComponent* locomotion = world_.TryGetLocomotion(entity);
                const GameplayAnimationLinkComponent* animLink = world_.TryGetAnimationLink(entity);
                if (locomotion == nullptr || animLink == nullptr || animLink->skinnedDrawIndex < 0)
                {
                    continue;
                }

                SkinnedDrawItem* skinnedItem = ctx.levelInstance->GetSkinnedDrawItem(*ctx.scene, animLink->skinnedDrawIndex);
                if (skinnedItem == nullptr || skinnedItem->controller.stateMachineAsset == nullptr)
                {
                    continue;
                }

                AnimationControllerRuntime& controller = skinnedItem->controller;

                SetAnimationFloatParameterByAliases_(
                    controller,
                    { "MoveX", "InputX", "LocomotionX", "DirectionX", "Strafe", "StrafeX" },
                    locomotion->moveX);

                SetAnimationFloatParameterByAliases_(
                    controller,
                    { "MoveY", "InputY", "LocomotionY", "DirectionY", "Forward", "ForwardY" },
                    locomotion->moveY);

                SetAnimationFloatParameterByAliases_(
                    controller,
                    { "Speed", "MoveSpeed", "PlanarSpeed", "GroundSpeed", "LocomotionSpeed" },
                    locomotion->planarSpeed);

                SetAnimationBoolParameterByAliases_(
                    controller,
                    { "IsMoving", "Moving", "bIsMoving" },
                    locomotion->isMoving);

                SetAnimationBoolParameterByAliases_(
                    controller,
                    { "IsRunning", "Running", "bIsRunning" },
                    locomotion->isRunning);
            }
        }

        void SyncGameplayActionsToAnimation_(const GameplayUpdateContext& ctx)
        {
            if (ctx.levelInstance == nullptr || ctx.scene == nullptr)
            {
                return;
            }

            for (const EntityHandle entity : nodeBoundEntities_)
            {
                GameplayActionComponent* action = world_.TryGetAction(entity);
                const GameplayAnimationLinkComponent* animLink = world_.TryGetAnimationLink(entity);
                if (action == nullptr || animLink == nullptr || animLink->skinnedDrawIndex < 0)
                {
                    continue;
                }

                SkinnedDrawItem* skinnedItem = ctx.levelInstance->GetSkinnedDrawItem(*ctx.scene, animLink->skinnedDrawIndex);
                if (skinnedItem == nullptr || skinnedItem->controller.stateMachineAsset == nullptr)
                {
                    continue;
                }

                AnimationControllerRuntime& controller = skinnedItem->controller;
                const bool hasRequest = action->requested != GameplayActionKind::None;
                const bool requestAttack = action->requested == GameplayActionKind::LightAttack;
                const bool requestInteract = action->requested == GameplayActionKind::Interact;
                const bool requestJump = action->requested == GameplayActionKind::Jump;
                const bool isAttacking = action->busy && action->current == GameplayActionKind::LightAttack;
                const bool isInteracting = action->busy && action->current == GameplayActionKind::Interact;
                const bool isJumping = action->busy && action->current == GameplayActionKind::Jump;

                SetAnimationBoolParameterByAliases_(
                    controller,
                    { "ActionRequested", "HasActionRequest", "WantsAction", "bActionRequested" },
                    hasRequest);

                SetAnimationBoolParameterByAliases_(
                    controller,
                    { "ActionBusy", "Busy", "IsBusy", "bBusy" },
                    action->busy);

                SetAnimationBoolParameterByAliases_(
                    controller,
                    { "LightAttackRequested", "AttackRequested", "WantsAttack", "RequestAttack" },
                    requestAttack);

                SetAnimationBoolParameterByAliases_(
                    controller,
                    { "InteractRequested", "UseRequested", "WantsInteract", "RequestInteract" },
                    requestInteract);

                SetAnimationBoolParameterByAliases_(
                    controller,
                    { "JumpRequested", "WantsJump", "RequestJump" },
                    requestJump);

                SetAnimationBoolParameterByAliases_(
                    controller,
                    { "IsAttacking", "Attacking", "bIsAttacking" },
                    isAttacking);

                SetAnimationBoolParameterByAliases_(
                    controller,
                    { "IsInteracting", "Interacting", "bIsInteracting" },
                    isInteracting);

                SetAnimationBoolParameterByAliases_(
                    controller,
                    { "IsJumping", "Jumping", "bIsJumping" },
                    isJumping);


                SetAnimationNumericParameterByAliases_(
                    controller,
                    { "RequestedAction", "RequestedActionKind", "ActionRequestKind", "ActionKindRequested" },
                    static_cast<int>(action->requested));

                SetAnimationNumericParameterByAliases_(
                    controller,
                    { "CurrentAction", "CurrentActionKind", "ActiveAction", "ActionKind" },
                    static_cast<int>(action->current));

                if (!action->requestDispatched && hasRequest)
                {
                    switch (action->requested)
                    {
                    case GameplayActionKind::LightAttack:
                        FireAnimationTriggerByAliases_(
                            controller,
                            { "LightAttack", "Attack", "LightAttackTrigger", "AttackTrigger" });
                        break;
                    case GameplayActionKind::Interact:
                        FireAnimationTriggerByAliases_(
                            controller,
                            { "Interact", "Use", "InteractTrigger", "UseTrigger" });
                        break;
                    case GameplayActionKind::Jump:
                        FireAnimationTriggerByAliases_(
                            controller,
                            { "Jump", "JumpTrigger", "StartJump" });
                        break;
                    case GameplayActionKind::None:
                    default:
                        break;
                    }

                    action->requestDispatched = true;
                }
            }
        }

        void SyncGameplayTransformsToRuntime_(const GameplayUpdateContext& ctx)
        {
            if (ctx.levelAsset == nullptr || ctx.levelInstance == nullptr || ctx.scene == nullptr)
            {
                return;
            }

            bool anyTransformChanged = false;
            for (const EntityHandle entity : nodeBoundEntities_)
            {
                const GameplayTransformComponent* transform = world_.TryGetTransform(entity);
                const GameplayNodeLinkComponent* nodeLink = world_.TryGetNodeLink(entity);
                if (transform == nullptr || nodeLink == nullptr)
                {
                    continue;
                }
                if (nodeLink->nodeIndex < 0 || static_cast<std::size_t>(nodeLink->nodeIndex) >= ctx.levelAsset->nodes.size())
                {
                    continue;
                }
                LevelNode& node = ctx.levelAsset->nodes[static_cast<std::size_t>(nodeLink->nodeIndex)];
                if (!node.alive)
                {
                    continue;
                }

                const Transform desiredTransform{
                    .position = transform->position,
                    .rotationDegrees = transform->rotationDegrees,
                    .scale = transform->scale
                };

                if (mathUtils::NearlyEqualVec3_(node.transform.position, desiredTransform.position) &&
                    mathUtils::NearlyEqualVec3_(node.transform.rotationDegrees, desiredTransform.rotationDegrees) &&
                    mathUtils::NearlyEqualVec3_(node.transform.scale, desiredTransform.scale))
                {
                    continue;
                }

                node.transform.position = desiredTransform.position;
                node.transform.rotationDegrees = desiredTransform.rotationDegrees;
                node.transform.scale = desiredTransform.scale;
                anyTransformChanged = true;
            }

            if (anyTransformChanged)
            {
                ctx.levelInstance->MarkTransformsDirty();
                ctx.levelInstance->SyncTransformsIfDirty(*ctx.levelAsset, *ctx.scene);
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
        std::vector<IntentBinding> intentBindings_{};
        std::vector<EntityHandle> motorEntities_{};
        std::vector<EntityHandle> nodeBoundEntities_{};
        std::vector<GameplayAnimationNotifyRecord> recentNotifyEvents_{};
    };
}