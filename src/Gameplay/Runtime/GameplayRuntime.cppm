module;

#include <algorithm>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

export module core:gameplay_runtime;

import :gameplay;
export import :gameplay_runtime_common;
import :gameplay_graph;
import :gameplay_graph_assets;
import :gameplay_input_system;
import :gameplay_bootstrap;
import :gameplay_scene_sync;
import :gameplay_follow_camera;
import :character_controller;
import :character_movement;
import :combat_system;
import :interaction_system;
import :gameplay_animation_bridge;
import :gameplay_animation_bridge_system;

export namespace rendern
{
    class GameplayRuntime
    {
    public:
        GameplayRuntime() = default;

        void Initialize(LevelAsset& levelAsset, LevelInstance& levelInstance, Scene& scene)
        {
            defaultGraphAsset_ = MakeDefaultHumanoidGameplayGraphAsset();

            GameplayUpdateContext ctx{};
            ctx.mode = GameplayRuntimeMode::Editor;
            ctx.levelAsset = &levelAsset;
            ctx.levelInstance = &levelInstance;
            ctx.scene = &scene;
            EnsureBootstrapEntity_(ctx);
            lastMode_ = GameplayRuntimeMode::Editor;
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
            lastMode_ = GameplayRuntimeMode::Editor;
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
                    if (ctx.mode != GameplayRuntimeMode::Game || ctx.input == nullptr)
                    {
                        return;
                    }

                    ReadKeyboardMouseGameplayIntent(*ctx.input, bindings, outIntent);
                });
        }

        void UnbindIntentSource(const EntityHandle entity)
        {
            intentBindings_.erase(
                std::remove_if(intentBindings_.begin(),
                    intentBindings_.end(),
                    [entity](const GameplayIntentBinding& binding)
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

                if (GameplayCharacterCommandComponent* command = world_.TryGetCharacterCommand(entity))
                {
                    *command = {};
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

            if (ctx.mode != lastMode_)
            {
                HandleRuntimeModeChanged_(ctx);
                lastMode_ = ctx.mode;
            }

            if (ctx.mode != GameplayRuntimeMode::Game)
            {
                return;
            }

            UpdateGameplayIntentSources(world_, intentBindings_, ctx);
            UpdateControlledFollowCamera_(ctx);
            BuildGameplayCharacterCommands(world_, nodeBoundEntities_, ctx);
            UpdateGameplayCombatRequests(world_, nodeBoundEntities_);
            UpdateGameplayInteractionRequests(world_, nodeBoundEntities_);
            ExecuteGameplayGraphs_(ctx);
            UpdateGameplayCharacterMovement(world_, nodeBoundEntities_, ctx.deltaSeconds);
            UpdateGameplayCharacterLocomotion(world_, nodeBoundEntities_);
            SyncGameplayTransformsToRuntime(world_, nodeBoundEntities_, ctx);
            PushGameplayStateToAnimation(world_, nodeBoundEntities_, ctx);
        }

        void PostAnimationUpdate(const GameplayUpdateContext& ctx)
        {
            if (ctx.mode != GameplayRuntimeMode::Game)
            {
                return;
            }

            ConsumeGameplayAnimationEvents(
                world_,
                nodeBoundEntities_,
                ctx,
                recentNotifyEvents_,
                recentGameplayEvents_);

            for (const GameplayEventRecord& eventRecord : recentGameplayEvents_)
            {
                auto it = graphInstances_.find(eventRecord.entity);
                if (it != graphInstances_.end())
                {
                    PushGameplayGraphEvent(it->second, eventRecord.gameplayEventId);
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

        [[nodiscard]] EntityHandle SpawnNodeBoundEntity(
            const GameplayUpdateContext& ctx,
            const int nodeIndex,
            const bool playerControlled)
        {
            if (ctx.levelAsset == nullptr || ctx.levelInstance == nullptr || ctx.scene == nullptr)
            {
                return kNullEntity;
            }

            EntityHandle entity = SpawnGameplayNodeBoundEntity(
                world_,
                nodeBoundEntities_,
                *ctx.levelAsset,
                *ctx.levelInstance,
                nodeIndex,
                playerControlled);
            if (entity == kNullEntity)
            {
                return kNullEntity;
            }

            CreateDefaultGraphInstance_(entity);
            if (playerControlled)
            {
                controlledEntity_ = entity;
                BindKeyboardMouseIntentSource(entity);
            }
            return entity;
        }

    private:
        [[nodiscard]] static bool EvaluateGraphTransition_(
            const GameplayGraphInstance& graph,
            const GameplayGraphTransitionDesc& transition)
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
                    [this](const GameplayIntentBinding& binding)
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
            for (GameplayIntentBinding& binding : intentBindings_)
            {
                if (binding.entity == entity)
                {
                    binding.callback = std::move(callback);
                    return;
                }
            }

            intentBindings_.push_back(GameplayIntentBinding{
                .entity = entity,
                .callback = std::move(callback)
            });
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
            SetGameplayGraphInt(instance.parameters, "requestedActionKind", 0);
            SetGameplayGraphInt(instance.parameters, "currentAction", 0);

            graphInstances_.insert_or_assign(entity, std::move(instance));
        }

        void SyncActionStateToGraphParameters_(const EntityHandle entity, GameplayGraphInstance& graph)
        {
            const GameplayActionComponent* action = world_.TryGetAction(entity);
            if (action == nullptr)
            {
                SetGameplayGraphBool(graph.parameters, "hasActionRequest", false);
                SetGameplayGraphBool(graph.parameters, "actionBusy", false);
                SetGameplayGraphInt(graph.parameters, "requestedActionKind", 0);
                SetGameplayGraphInt(graph.parameters, "currentAction", 0);
                return;
            }

            SetGameplayGraphBool(graph.parameters, "hasActionRequest", action->requested != GameplayActionKind::None);
            SetGameplayGraphBool(graph.parameters, "actionBusy", action->busy);
            SetGameplayGraphInt(graph.parameters, "requestedActionKind", static_cast<int>(action->requested));
            SetGameplayGraphInt(graph.parameters, "currentAction", static_cast<int>(action->current));
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
                SyncActionStateToGraphParameters_(entity, graph);

                for (std::size_t layerIndex = 0; layerIndex < graph.layers.size() && layerIndex < graph.asset->layers.size(); ++layerIndex)
                {
                    GameplayGraphLayerRuntimeState& runtimeLayer = graph.layers[layerIndex];
                    const GameplayGraphLayerDesc& assetLayer = graph.asset->layers[layerIndex];
                    ExecuteGraphLayer_(entity, graph, runtimeLayer, assetLayer, ctx);
                }

                SyncActionStateToGraphParameters_(entity, graph);
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
                ExecuteGraphTasks_(entity, graph, *state, state->onEnter);
                runtimeLayer.enterPending = false;
            }

            ExecuteGraphTasks_(entity, graph, *state, state->onUpdate);

            for (const GameplayGraphTransitionDesc& transition : state->transitions)
            {
                if (!EvaluateGraphTransition_(graph, transition))
                {
                    continue;
                }

                ExecuteGraphTasks_(entity, graph, *state, state->onExit);

                runtimeLayer.previousStateIndex = runtimeLayer.activeStateIndex;
                runtimeLayer.activeStateIndex = FindGameplayGraphStateIndex(assetLayer, transition.toState);
                runtimeLayer.stateTime = 0.0f;
                runtimeLayer.enterPending = true;

                if (runtimeLayer.activeStateIndex >= 0 &&
                    static_cast<std::size_t>(runtimeLayer.activeStateIndex) < assetLayer.states.size())
                {
                    const GameplayGraphStateDesc& newState = assetLayer.states[static_cast<std::size_t>(runtimeLayer.activeStateIndex)];
                    ExecuteGraphTasks_(entity, graph, newState, newState.onEnter);
                    runtimeLayer.enterPending = false;
                }
                return;
            }

            runtimeLayer.stateTime += std::max(ctx.deltaSeconds, 0.0f);
        }

        void ExecuteGraphTasks_(
            const EntityHandle entity,
            GameplayGraphInstance& graph,
            const GameplayGraphStateDesc& state,
            const std::vector<GameplayGraphTaskDesc>& tasks)
        {
            for (const GameplayGraphTaskDesc& task : tasks)
            {
                const std::string taskName = CanonicalizeGameplayGraphToken(task.name);
                if (taskName == "beginactionstate")
                {
                    BeginActionState_(entity, graph);
                }
                [[maybe_unused]] const auto& unusedState = state;
            }
        }

        void BeginActionState_(const EntityHandle entity, GameplayGraphInstance& graph)
        {
            GameplayActionComponent* action = world_.TryGetAction(entity);
            if (action == nullptr || action->requested == GameplayActionKind::None)
            {
                return;
            }

            action->busy = true;
            if (action->current == GameplayActionKind::None)
            {
                action->current = action->requested;
            }

            SetGameplayGraphBool(graph.parameters, "hasActionRequest", true);
            SetGameplayGraphBool(graph.parameters, "actionBusy", action->busy);
            SetGameplayGraphInt(graph.parameters, "requestedActionKind", static_cast<int>(action->requested));
            SetGameplayGraphInt(graph.parameters, "currentAction", static_cast<int>(action->current));
        }

        void ResetSimulationState_()
        {
            for (const EntityHandle entity : nodeBoundEntities_)
            {
                if (GameplayInputIntentComponent* intent = world_.TryGetInputIntent(entity))
                {
                    *intent = {};
                }

                if (GameplayCharacterCommandComponent* command = world_.TryGetCharacterCommand(entity))
                {
                    *command = {};
                }

                if (GameplayCharacterMotorComponent* motor = world_.TryGetCharacterMotor(entity))
                {
                    motor->velocity = {};
                    motor->desiredMoveWorld = {};
                }

                if (GameplayCharacterMovementStateComponent* movementState = world_.TryGetCharacterMovementState(entity))
                {
                    movementState->grounded = true;
                    movementState->jumping = false;
                    movementState->falling = false;
                    movementState->desiredFacingYawDegrees = movementState->facingYawDegrees;
                    movementState->previousFacingYawDegrees = movementState->facingYawDegrees;
                }

                if (GameplayLocomotionComponent* locomotion = world_.TryGetLocomotion(entity))
                {
                    *locomotion = {};
                }

                if (GameplayActionComponent* action = world_.TryGetAction(entity))
                {
                    action->requested = GameplayActionKind::None;
                    action->current = GameplayActionKind::None;
                    action->busy = false;
                    action->requestDispatched = false;
                }

                if (GameplayAnimationNotifyStateComponent* notifyState = world_.TryGetAnimationNotifyState(entity))
                {
                    *notifyState = {};
                }

                if (auto it = graphInstances_.find(entity); it != graphInstances_.end())
                {
                    ClearGameplayGraphFrameState(it->second);
                    SyncActionStateToGraphParameters_(entity, it->second);
                }
            }
        }

        void UpdateControlledFollowCamera_(const GameplayUpdateContext& ctx)
        {
            if (controlledEntity_ == kNullEntity || !world_.IsEntityValid(controlledEntity_))
            {
                return;
            }

            followCameraController_.Update(world_, controlledEntity_, ctx);
        }

        void HandleRuntimeModeChanged_(const GameplayUpdateContext& ctx)
        {
            recentNotifyEvents_.clear();
            recentGameplayEvents_.clear();
            if (controlledEntity_ != kNullEntity && world_.IsEntityValid(controlledEntity_))
            {
                followCameraController_.Reset(world_, controlledEntity_);
            }

            if (ctx.mode == GameplayRuntimeMode::Editor)
            {
                ResetSimulationState_();
                if (ctx.scene != nullptr && ctx.levelInstance != nullptr)
                {
                    PushGameplayStateToAnimation(world_, nodeBoundEntities_, ctx);
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

            const int bootstrapNodeIndex = FindGameplayBootstrapNodeIndex(*ctx.levelAsset, *ctx.levelInstance);
            if (bootstrapNodeIndex < 0)
            {
                return;
            }

            SpawnNodeBoundEntity(ctx, bootstrapNodeIndex, true);
        }

    private:
        GameplayWorld world_{};
        EntityHandle controlledEntity_{ kNullEntity };
        std::vector<GameplayIntentBinding> intentBindings_{};
        std::vector<EntityHandle> nodeBoundEntities_{};
        std::unordered_map<EntityHandle, GameplayGraphInstance> graphInstances_{};
        GameplayGraphAsset defaultGraphAsset_{};
        GameplayFollowCameraController followCameraController_{};
        GameplayRuntimeMode lastMode_{ GameplayRuntimeMode::Editor };
        std::vector<GameplayAnimationNotifyRecord> recentNotifyEvents_{};
        std::vector<GameplayEventRecord> recentGameplayEvents_{};
    };
}
