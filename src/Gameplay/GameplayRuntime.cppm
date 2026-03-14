module;

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstddef>
#include <string>
#include <utility>
#include <string_view>
#include <functional>
#include <unordered_map>
#include <vector>

export module core:gameplay_runtime;

import :gameplay;
import :gameplay_graph;
import :input;
import :level;
import :scene;
import :animation_controller;
import :math_utils;
import :gameplay_animation_bridge;

export namespace rendern
{
    struct GameplayAnimationNotifyRecord
    {
        EntityHandle entity{ kNullEntity };
        int nodeIndex{ -1 };
        int skinnedDrawIndex{ -1 };
        AnimationNotifyEvent event{};
    };

    struct GameplayEventRecord
    {
        EntityHandle entity{ kNullEntity };
        int nodeIndex{ -1 };
        int skinnedDrawIndex{ -1 };
        std::uint64_t sequence{ 0 };
        std::string animationEventId{};
        std::string gameplayEventId{};
        std::string stateName{};
        std::string clipName{};
        float normalizedTime{ 0.0f };
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
            defaultGraphAsset_ = MakeDefaultHumanoidGameplayGraphAsset();
            defaultAnimationBindingAsset_ = MakeDefaultHumanoidGameplayAnimationBindingAsset();

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
            recentGameplayEvents_.clear();
            intentBindings_.clear();
            nodeBoundEntities_.clear();
            graphInstances_.clear();
            controlledEntity_ = kNullEntity;
        }

        void BindIntentSource(const EntityHandle entity, GameplayIntentSourceCallback callback)
        {
            recentNotifyEvents_.clear();
            recentGameplayEvents_.clear();

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
                    [[maybe_unused]] GameplayActionComponent* action)
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
            recentGameplayEvents_.clear();
            CompactTrackedState_();

            for (const EntityHandle entity : nodeBoundEntities_)
            {
                if (GameplayInputIntentComponent* intent = world_.TryGetInputIntent(entity))
                {
                    *intent = {};
                }

                if (GameplayAnimationNotifyStateComponent* notifyState = world_.TryGetAnimationNotifyState(entity))
                {
                    ResetGameplayAnimationNotifyFrame(*notifyState);
                }

                if (auto it = graphInstances_.find(entity); it != graphInstances_.end())
                {
                    ClearGameplayGraphFrameState(it->second);
                }
            }
        }

        void PreAnimationUpdate(const GameplayUpdateContext& ctx)
        {
            EnsureBootstrapEntity_(ctx);
            UpdateIntentSources_(ctx);
            ExecuteGameplayGraphs_(ctx);
            SyncGameplayTransformsToRuntime_(ctx);
            SyncGameplayGraphsToAnimation_(ctx);
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

                for (const AnimationNotifyEvent& event : events)
                {
                    recentNotifyEvents_.push_back(GameplayAnimationNotifyRecord{
                        .entity = entity,
                        .nodeIndex = nodeLink->nodeIndex,
                        .skinnedDrawIndex = animLink->skinnedDrawIndex,
                        .event = event
                    });
                }

                RouteAnimationEventsToGameplay_(
                    entity,
                    nodeLink->nodeIndex,
                    animLink->skinnedDrawIndex,
                    skinnedItem->controller,
                    *notifyState,
                    events);
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

        [[nodiscard]] const std::vector<EntityHandle>& GetNodeBoundEntities() const noexcept
        {
            return nodeBoundEntities_;
        }

        [[nodiscard]] const std::vector<GameplayAnimationNotifyRecord>& GetRecentNotifyEvents() const noexcept
        {
            return recentNotifyEvents_;
        }

        [[nodiscard]] const std::vector<GameplayEventRecord>& GetRecentGameplayEvents() const noexcept
        {
            return recentGameplayEvents_;
        }

        [[nodiscard]] EntityHandle SpawnNodeBoundEntity(const GameplayUpdateContext& ctx, const int nodeIndex, const bool playerControlled = false)
        {
            if (ctx.levelAsset == nullptr || nodeIndex < 0 || static_cast<std::size_t>(nodeIndex) >= ctx.levelAsset->nodes.size())
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
            world_.AddAnimationNotifyState(entity);

            TrackNodeBoundEntity_(entity);
            CreateDefaultGraphInstance_(entity);

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

            for (auto it = graphInstances_.begin(); it != graphInstances_.end(); )
            {
                if (!world_.IsEntityValid(it->first))
                {
                    it = graphInstances_.erase(it);
                }
                else
                {
                    ++it;
                }
            }

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

        void CreateDefaultGraphInstance_(const EntityHandle entity)
        {
            GameplayGraphInstance instance{};
            instance.asset = &defaultGraphAsset_;
            instance.layers.reserve(defaultGraphAsset_.layers.size());

            for (const GameplayGraphLayerDesc& layer : defaultGraphAsset_.layers)
            {
                const int defaultStateIndex = FindGameplayGraphStateIndex(layer, layer.defaultState);
                instance.layers.push_back(GameplayGraphLayerRuntimeState{
                    .activeStateIndex = defaultStateIndex,
                    .previousStateIndex = -1,
                    .stateTime = 0.0f,
                    .enterPending = true
                });
            }

            SetGameplayGraphBool(instance.parameters, "hasActionRequest", false);
            SetGameplayGraphBool(instance.parameters, "actionBusy", false);
            SetGameplayGraphBool(instance.parameters, "actionRequestDispatched", false);
            SetGameplayGraphInt(instance.parameters, "requestedActionKind", 0);
            SetGameplayGraphInt(instance.parameters, "currentAction", 0);

            graphInstances_.insert_or_assign(entity, std::move(instance));
        }

        void UpdateIntentSources_(const GameplayUpdateContext& ctx)
        {
            for (const IntentBinding& binding : intentBindings_)
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

                *intent = {};
                binding.callback(binding.entity, ctx, world_, *intent, nullptr);
            }
        }

        void ExecuteGameplayGraphs_(const GameplayUpdateContext& ctx)
        {
            for (const EntityHandle entity : nodeBoundEntities_)
            {
                auto it = graphInstances_.find(entity);
                if (it == graphInstances_.end())
                {
                    continue;
                }

                GameplayGraphInstance& graph = it->second;
                SyncInputToGraphParameters_(entity, graph);

                for (std::size_t layerIndex = 0; layerIndex < graph.layers.size() && layerIndex < graph.asset->layers.size(); ++layerIndex)
                {
                    GameplayGraphLayerRuntimeState& runtimeLayer = graph.layers[layerIndex];
                    const GameplayGraphLayerDesc& assetLayer = graph.asset->layers[layerIndex];
                    ExecuteGraphLayer_(entity, graph, runtimeLayer, assetLayer, ctx);
                }
            }
        }

        void ExecuteGraphLayer_(
            const EntityHandle entity,
            GameplayGraphInstance& graph,
            GameplayGraphLayerRuntimeState& runtimeLayer,
            const GameplayGraphLayerDesc& assetLayer,
            const GameplayUpdateContext& ctx)
        {
            if (runtimeLayer.activeStateIndex < 0 ||
                static_cast<std::size_t>(runtimeLayer.activeStateIndex) >= assetLayer.states.size())
            {
                runtimeLayer.activeStateIndex = FindGameplayGraphStateIndex(assetLayer, assetLayer.defaultState);
                runtimeLayer.enterPending = true;
                runtimeLayer.stateTime = 0.0f;
            }
            if (runtimeLayer.activeStateIndex < 0)
            {
                return;
            }

            const GameplayGraphStateDesc* state = &assetLayer.states[static_cast<std::size_t>(runtimeLayer.activeStateIndex)];
            if (runtimeLayer.enterPending)
            {
                ExecuteGraphTasks_(entity, graph, runtimeLayer, *state, state->onEnter, ctx);
                runtimeLayer.enterPending = false;
            }

            ExecuteGraphTasks_(entity, graph, runtimeLayer, *state, state->onUpdate, ctx);

            for (const GameplayGraphTransitionDesc& transition : state->transitions)
            {
                if (!EvaluateGraphTransition_(graph, transition))
                {
                    continue;
                }

                ExecuteGraphTasks_(entity, graph, runtimeLayer, *state, state->onExit, ctx);

                runtimeLayer.previousStateIndex = runtimeLayer.activeStateIndex;
                runtimeLayer.activeStateIndex = FindGameplayGraphStateIndex(assetLayer, transition.toState);
                runtimeLayer.stateTime = 0.0f;
                runtimeLayer.enterPending = true;

                if (runtimeLayer.activeStateIndex >= 0 &&
                    static_cast<std::size_t>(runtimeLayer.activeStateIndex) < assetLayer.states.size())
                {
                    const GameplayGraphStateDesc& newState = assetLayer.states[static_cast<std::size_t>(runtimeLayer.activeStateIndex)];
                    ExecuteGraphTasks_(entity, graph, runtimeLayer, newState, newState.onEnter, ctx);
                    runtimeLayer.enterPending = false;
                }
                return;
            }

            runtimeLayer.stateTime += std::max(ctx.deltaSeconds, 0.0f);
        }

        [[nodiscard]] bool EvaluateGraphTransition_(const GameplayGraphInstance& graph, const GameplayGraphTransitionDesc& transition) const
        {
            for (const GameplayGraphConditionDesc& condition : transition.conditions)
            {
                const std::string conditionName = CanonicalizeGameplayGraphToken(condition.name);
                if (conditionName == "booltrue")
                {
                    if (!GetGameplayGraphBool(graph.parameters, condition.parameter, false))
                    {
                        return false;
                    }
                    continue;
                }
                if (conditionName == "boolfalse")
                {
                    if (GetGameplayGraphBool(graph.parameters, condition.parameter, false))
                    {
                        return false;
                    }
                    continue;
                }
                if (conditionName == "floatgreater")
                {
                    if (!(GetGameplayGraphFloat(graph.parameters, condition.parameter, 0.0f) > condition.threshold))
                    {
                        return false;
                    }
                    continue;
                }
                if (conditionName == "floatless")
                {
                    if (!(GetGameplayGraphFloat(graph.parameters, condition.parameter, 0.0f) < condition.threshold))
                    {
                        return false;
                    }
                    continue;
                }

                return false;
            }

            return true;
        }

        void ExecuteGraphTasks_(
            const EntityHandle entity,
            GameplayGraphInstance& graph,
            GameplayGraphLayerRuntimeState& runtimeLayer,
            const GameplayGraphStateDesc& state,
            const std::vector<GameplayGraphTaskDesc>& tasks,
            const GameplayUpdateContext& ctx)
        {
            for (const GameplayGraphTaskDesc& task : tasks)
            {
                const std::string taskName = CanonicalizeGameplayGraphToken(task.name);
                if (taskName == "syncinputtoparameters")
                {
                    SyncInputToGraphParameters_(entity, graph);
                }
                else if (taskName == "queueactionrequestsfrominput")
                {
                    QueueActionRequestsFromInput_(graph);
                }
                else if (taskName == "buildcamerarelativemove")
                {
                    BuildCameraRelativeMove_(entity, graph, ctx);
                }
                else if (taskName == "applycharactermotor")
                {
                    ApplyCharacterMotor_(entity, graph, ctx);
                }
                else if (taskName == "facemovementforwardonly")
                {
                    FaceMovementForwardOnly_(entity, graph);
                }
                else if (taskName == "computelocomotionmetrics")
                {
                    ComputeLocomotionMetrics_(entity, graph);
                }
                else if (taskName == "beginactionstate")
                {
                    BeginActionState_(graph);
                }
                [[maybe_unused]] const auto& unusedRuntimeLayer = runtimeLayer;
                [[maybe_unused]] const auto& unusedState = state;
            }
        }

        void SyncInputToGraphParameters_(const EntityHandle entity, GameplayGraphInstance& graph)
        {
            const GameplayInputIntentComponent* intent = world_.TryGetInputIntent(entity);
            if (intent == nullptr)
            {
                return;
            }

            SetGameplayGraphFloat(graph.parameters, "moveX", intent->moveX);
            SetGameplayGraphFloat(graph.parameters, "moveY", intent->moveY);
            SetGameplayGraphBool(graph.parameters, "runHeld", intent->runHeld);
            if (intent->jumpPressed)
            {
                SetGameplayGraphTrigger(graph.parameters, "jumpPressed");
            }
            if (intent->attackPressed)
            {
                SetGameplayGraphTrigger(graph.parameters, "attackPressed");
            }
            if (intent->interactPressed)
            {
                SetGameplayGraphTrigger(graph.parameters, "interactPressed");
            }
        }

        void QueueActionRequestsFromInput_(GameplayGraphInstance& graph)
        {
            const bool actionBusy = GetGameplayGraphBool(graph.parameters, "actionBusy", false);
            const bool hasActionRequest = GetGameplayGraphBool(graph.parameters, "hasActionRequest", false);
            if (actionBusy || hasActionRequest)
            {
                return;
            }

            int requestedActionKind = 0;
            if (ConsumeGameplayGraphTrigger(graph.parameters, "jumpPressed"))
            {
                requestedActionKind = 3;
            }
            else if (ConsumeGameplayGraphTrigger(graph.parameters, "attackPressed"))
            {
                requestedActionKind = 1;
            }
            else if (ConsumeGameplayGraphTrigger(graph.parameters, "interactPressed"))
            {
                requestedActionKind = 2;
            }

            if (requestedActionKind == 0)
            {
                return;
            }

            SetGameplayGraphInt(graph.parameters, "requestedActionKind", requestedActionKind);
            SetGameplayGraphBool(graph.parameters, "hasActionRequest", true);
            SetGameplayGraphBool(graph.parameters, "actionRequestDispatched", false);
        }

        void BuildCameraRelativeMove_(const EntityHandle entity, GameplayGraphInstance& graph, const GameplayUpdateContext& ctx)
        {
            GameplayCharacterMotorComponent* motor = world_.TryGetCharacterMotor(entity);
            if (motor == nullptr || ctx.scene == nullptr)
            {
                return;
            }

            const float moveX = GetGameplayGraphFloat(graph.parameters, "moveX", 0.0f);
            const float moveY = GetGameplayGraphFloat(graph.parameters, "moveY", 0.0f);

            mathUtils::Vec3 basisRight(1.0f, 0.0f, 0.0f);
            mathUtils::Vec3 basisForward(0.0f, 0.0f, 1.0f);
            BuildPlanarMovementBasis_(ctx.scene->camera, basisRight, basisForward);

            mathUtils::Vec3 desiredMoveWorld = basisRight * moveX + basisForward * moveY;
            if (mathUtils::Length(desiredMoveWorld) > 1e-6f)
            {
                desiredMoveWorld = mathUtils::Normalize(desiredMoveWorld);
            }
            else
            {
                desiredMoveWorld = mathUtils::Vec3(0.0f, 0.0f, 0.0f);
            }

            motor->desiredMoveWorld = desiredMoveWorld;
        }

        void ApplyCharacterMotor_(const EntityHandle entity, GameplayGraphInstance& graph, const GameplayUpdateContext& ctx)
        {
            GameplayTransformComponent* transform = world_.TryGetTransform(entity);
            GameplayCharacterMotorComponent* motor = world_.TryGetCharacterMotor(entity);
            if (transform == nullptr || motor == nullptr)
            {
                return;
            }

            const bool runHeld = GetGameplayGraphBool(graph.parameters, "runHeld", false);
            const float targetSpeed = runHeld ? motor->maxRunSpeed : motor->maxWalkSpeed;
            const mathUtils::Vec3 targetVelocity = motor->desiredMoveWorld * targetSpeed;
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
        }

        void FaceMovementForwardOnly_(const EntityHandle entity, GameplayGraphInstance& graph)
        {
            GameplayTransformComponent* transform = world_.TryGetTransform(entity);
            GameplayCharacterMotorComponent* motor = world_.TryGetCharacterMotor(entity);
            if (transform == nullptr || motor == nullptr)
            {
                return;
            }

            const float moveY = GetGameplayGraphFloat(graph.parameters, "moveY", 0.0f);
            const float planarSpeed = mathUtils::Length(motor->velocity);
            const bool isMoving = planarSpeed > 1e-4f;
            if (isMoving && moveY > 0.1f)
            {
                transform->rotationDegrees.y = mathUtils::RadToDeg(std::atan2(motor->velocity.x, motor->velocity.z));
            }
        }

        void ComputeLocomotionMetrics_(const EntityHandle entity, GameplayGraphInstance& graph)
        {
            const GameplayTransformComponent* transform = world_.TryGetTransform(entity);
            const GameplayCharacterMotorComponent* motor = world_.TryGetCharacterMotor(entity);
            if (transform == nullptr || motor == nullptr)
            {
                return;
            }

            const float planarSpeed = mathUtils::Length(motor->velocity);
            const bool isMoving = planarSpeed > 1e-4f;
            const float yawRadians = mathUtils::DegToRad(transform->rotationDegrees.y);
            const mathUtils::Vec3 actorForward(std::sin(yawRadians), 0.0f, std::cos(yawRadians));
            const mathUtils::Vec3 actorRight(actorForward.z, 0.0f, -actorForward.x);
            const float forwardSpeed = mathUtils::Dot(motor->velocity, actorForward);
            const float rightSpeed = mathUtils::Dot(motor->velocity, actorRight);
            const float moveX = GetGameplayGraphFloat(graph.parameters, "moveX", 0.0f);
            const bool wantsTurnInPlaceLeft = !isMoving && moveX < -0.5f;
            const bool wantsTurnInPlaceRight = !isMoving && moveX > 0.5f;

            const float previousYaw = GetGameplayGraphFloat(graph.blackboard, "previousYawDegrees", transform->rotationDegrees.y);
            float turnDeltaYawDegrees = transform->rotationDegrees.y - previousYaw;
            while (turnDeltaYawDegrees > 180.0f) turnDeltaYawDegrees -= 360.0f;
            while (turnDeltaYawDegrees < -180.0f) turnDeltaYawDegrees += 360.0f;

            SetGameplayGraphFloat(graph.parameters, "forwardSpeed", forwardSpeed);
            SetGameplayGraphFloat(graph.parameters, "rightSpeed", rightSpeed);
            SetGameplayGraphFloat(graph.parameters, "planarSpeed", planarSpeed);
            SetGameplayGraphFloat(graph.parameters, "turnDeltaYawDegrees", turnDeltaYawDegrees);
            SetGameplayGraphBool(graph.parameters, "isMoving", isMoving);
            SetGameplayGraphBool(graph.parameters, "isRunning", isMoving && GetGameplayGraphBool(graph.parameters, "runHeld", false));
            SetGameplayGraphBool(graph.parameters, "wantsTurnInPlaceLeft", wantsTurnInPlaceLeft);
            SetGameplayGraphBool(graph.parameters, "wantsTurnInPlaceRight", wantsTurnInPlaceRight);
            SetGameplayGraphFloat(graph.blackboard, "previousYawDegrees", transform->rotationDegrees.y);
        }

        void BeginActionState_(GameplayGraphInstance& graph)
        {
            const bool hasActionRequest = GetGameplayGraphBool(graph.parameters, "hasActionRequest", false);
            if (!hasActionRequest)
            {
                return;
            }

            SetGameplayGraphBool(graph.parameters, "actionBusy", true);
            SetGameplayGraphInt(graph.parameters, "currentAction", GetGameplayGraphInt(graph.parameters, "requestedActionKind", 0));
        }

        void SyncGameplayGraphsToAnimation_(const GameplayUpdateContext& ctx)
        {
            if (ctx.levelInstance == nullptr || ctx.scene == nullptr)
            {
                return;
            }

            for (const EntityHandle entity : nodeBoundEntities_)
            {
                auto graphIt = graphInstances_.find(entity);
                if (graphIt == graphInstances_.end())
                {
                    continue;
                }

                const GameplayAnimationLinkComponent* animLink = world_.TryGetAnimationLink(entity);
                if (animLink == nullptr || animLink->skinnedDrawIndex < 0)
                {
                    continue;
                }

                SkinnedDrawItem* skinnedItem = ctx.levelInstance->GetSkinnedDrawItem(*ctx.scene, animLink->skinnedDrawIndex);
                if (skinnedItem == nullptr || skinnedItem->controller.stateMachineAsset == nullptr)
                {
                    continue;
                }

                GameplayGraphInstance& graph = graphIt->second;
                WriteGraphParametersToAnimation_(skinnedItem->controller, graph.parameters);
                DispatchGraphAnimationTriggers_(skinnedItem->controller, graph);
            }
        }

        void WriteGraphParametersToAnimation_(AnimationControllerRuntime& controller, const GameplayGraphParameterStore& params)
        {
            for (const GameplayAnimationParameterBindingDesc& binding : defaultAnimationBindingAsset_.parameterBindings)
            {
                const auto it = params.values.find(binding.gameplayParameter);
                if (it == params.values.end())
                {
                    continue;
                }

                const GameplayGraphValue& value = it->second;
                switch (value.type)
                {
                case GameplayGraphValueType::Bool:
                case GameplayGraphValueType::Trigger:
                    SetAnimationParameter(controller.parameters, binding.animationParameter, value.boolValue);
                    break;
                case GameplayGraphValueType::Int:
                    SetAnimationParameter(controller.parameters, binding.animationParameter, value.intValue);
                    break;
                case GameplayGraphValueType::Float:
                    SetAnimationParameter(controller.parameters, binding.animationParameter, value.floatValue);
                    break;
                case GameplayGraphValueType::String:
                default:
                    break;
                }
            }
        }

        void DispatchGraphAnimationTriggers_(AnimationControllerRuntime& controller, GameplayGraphInstance& graph)
        {
            const bool hasActionRequest = GetGameplayGraphBool(graph.parameters, "hasActionRequest", false);
            const bool dispatched = GetGameplayGraphBool(graph.parameters, "actionRequestDispatched", false);
            if (!hasActionRequest || dispatched)
            {
                return;
            }

            const int requestedAction = GetGameplayGraphInt(graph.parameters, "requestedActionKind", 0);
            for (const GameplayAnimationEventTriggerBindingDesc& binding : defaultAnimationBindingAsset_.actionTriggerBindings)
            {
                if (binding.actionKind == requestedAction && !binding.animationTrigger.empty())
                {
                    FireAnimationTrigger(controller.parameters, binding.animationTrigger);
                    SetGameplayGraphBool(graph.parameters, "actionRequestDispatched", true);
                    return;
                }
            }
        }

        void RouteAnimationEventsToGameplay_(
            const EntityHandle entity,
            const int nodeIndex,
            const int skinnedDrawIndex,
            const AnimationControllerRuntime& controller,
            GameplayAnimationNotifyStateComponent& notifyState,
            const std::vector<AnimationNotifyEvent>& events)
        {
            auto graphIt = graphInstances_.find(entity);
            GameplayGraphInstance* graph = graphIt != graphInstances_.end() ? &graphIt->second : nullptr;

            for (const AnimationNotifyEvent& event : events)
            {
                ApplyAnimationNotifyToGameplayState(notifyState, nullptr, event);

                std::vector<std::string> gameplayEventIds{};
                CollectGameplayEventIdsForAnimationEvent(controller.stateMachineAsset, event, gameplayEventIds);

                if (gameplayEventIds.empty())
                {
                    gameplayEventIds.push_back(event.id);
                }

                for (const std::string& gameplayEventId : gameplayEventIds)
                {
                    if (graph != nullptr)
                    {
                        PushGameplayGraphEvent(*graph, gameplayEventId);
                        ApplyAnimationEventToGraphState_(*graph, gameplayEventId);
                    }

                    recentGameplayEvents_.push_back(GameplayEventRecord{
                        .entity = entity,
                        .nodeIndex = nodeIndex,
                        .skinnedDrawIndex = skinnedDrawIndex,
                        .sequence = event.sequence,
                        .animationEventId = event.id,
                        .gameplayEventId = gameplayEventId,
                        .stateName = event.stateName,
                        .clipName = event.clipName,
                        .normalizedTime = event.normalizedTime
                    });
                }
            }
        }

        void ApplyAnimationEventToGraphState_(GameplayGraphInstance& graph, std::string_view gameplayEventId)
        {
            const std::string canonical = CanonicalizeGameplayGraphToken(gameplayEventId);
            if (canonical.find("actionbegin") != std::string::npos)
            {
                SetGameplayGraphBool(graph.parameters, "actionBusy", true);
                SetGameplayGraphInt(graph.parameters, "currentAction", GetGameplayGraphInt(graph.parameters, "requestedActionKind", 0));
                return;
            }

            if (canonical.find("actionend") != std::string::npos || canonical.find("actionfinish") != std::string::npos)
            {
                SetGameplayGraphBool(graph.parameters, "actionBusy", false);
                SetGameplayGraphInt(graph.parameters, "currentAction", 0);
                SetGameplayGraphInt(graph.parameters, "requestedActionKind", 0);
                SetGameplayGraphBool(graph.parameters, "hasActionRequest", false);
                SetGameplayGraphBool(graph.parameters, "actionRequestDispatched", false);
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
        std::vector<EntityHandle> nodeBoundEntities_{};
        std::unordered_map<EntityHandle, GameplayGraphInstance> graphInstances_{};
        GameplayGraphAsset defaultGraphAsset_{};
        GameplayAnimationBindingAsset defaultAnimationBindingAsset_{};
        std::vector<GameplayAnimationNotifyRecord> recentNotifyEvents_{};
        std::vector<GameplayEventRecord> recentGameplayEvents_{};
    };
}
