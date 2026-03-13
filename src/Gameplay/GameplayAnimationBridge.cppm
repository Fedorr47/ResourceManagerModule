module;

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <string_view>
#include <initializer_list>

export module core:gameplay_animation_bridge;

import :gameplay;
import :animation_controller;

export namespace rendern
{
    namespace detail
    {
        [[nodiscard]] inline std::string CanonicalizeAnimationNameToken_(std::string_view name)
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

        [[nodiscard]] inline bool NameMatchesAnyAlias_(
            const std::string_view candidate,
            const std::initializer_list<std::string_view> aliases)
        {
            const std::string canonicalCandidate = CanonicalizeAnimationNameToken_(candidate);
            for (const std::string_view alias : aliases)
            {
                if (canonicalCandidate == CanonicalizeAnimationNameToken_(alias))
                {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] inline const AnimationParameterDesc* FindAnimationParameterByAliases_(
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
                if (NameMatchesAnyAlias_(desc.name, aliases))
                {
                    return &desc;
                }
            }
            return nullptr;
        }

        inline void SetAnimationFloatParameterByAliases_(
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

        inline void SetAnimationBoolParameterByAliases_(
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

        inline void SetAnimationIntParameterByAliases_(
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

        inline void SetAnimationNumericParameterByAliases_(
            AnimationControllerRuntime& controller,
            const std::initializer_list<std::string_view> aliases,
            const int value)
        {
            SetAnimationIntParameterByAliases_(controller, aliases, value);
            SetAnimationFloatParameterByAliases_(controller, aliases, static_cast<float>(value));
        }

        inline void FireAnimationTriggerByAliases_(
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

        [[nodiscard]] inline GameplayActionKind InferGameplayActionKindFromNotifyId_(const std::string_view notifyId) noexcept
        {
            if (NameMatchesAnyAlias_(
                notifyId,
                { "LightAttackBegin", "LightAttackStart", "AttackBegin", "AttackStart", "Attack" }))
            {
                return GameplayActionKind::LightAttack;
            }

            if (NameMatchesAnyAlias_(
                notifyId,
                { "InteractBegin", "InteractStart", "UseBegin", "UseStart", "Use" }))
            {
                return GameplayActionKind::Interact;
            }

            if (NameMatchesAnyAlias_(
                notifyId,
                { "JumpBegin", "JumpStart", "Jump" }))
            {
                return GameplayActionKind::Jump;
            }

            return GameplayActionKind::None;
        }
    }

    inline void ResetGameplayAnimationNotifyFrame(GameplayAnimationNotifyStateComponent& notifyState)
    {
        notifyState.anyThisFrame = false;
        notifyState.footstepThisFrame = false;
        notifyState.interactionPointThisFrame = false;
        notifyState.actionStartedThisFrame = false;
        notifyState.actionFinishedThisFrame = false;
        notifyState.hitWindowOpenedThisFrame = false;
        notifyState.hitWindowClosedThisFrame = false;
    }

    inline void ApplyAnimationNotifyToGameplayState(
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

        if (detail::NameMatchesAnyAlias_(event.id, { "Footstep", "Step", "FootStep" }))
        {
            notifyState.footstepThisFrame = true;
        }

        if (detail::NameMatchesAnyAlias_(event.id, { "Interact", "Interaction", "InteractionPoint", "UsePoint" }))
        {
            notifyState.interactionPointThisFrame = true;
        }

        if (detail::NameMatchesAnyAlias_(
            event.id,
            { "HitWindowBegin", "HitWindowStart", "AttackWindowBegin", "AttackWindowStart", "HitEnable", "EnableHit", "DamageWindowBegin", "DamageOn" }))
        {
            notifyState.hitWindowOpenedThisFrame = true;
            notifyState.hitWindowActive = true;
        }

        if (detail::NameMatchesAnyAlias_(
            event.id,
            { "HitWindowEnd", "HitWindowStop", "AttackWindowEnd", "AttackWindowStop", "HitDisable", "DisableHit", "DamageWindowEnd", "DamageOff" }))
        {
            notifyState.hitWindowClosedThisFrame = true;
            notifyState.hitWindowActive = false;
        }

        if (detail::NameMatchesAnyAlias_(
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
                    startedKind = detail::InferGameplayActionKindFromNotifyId_(event.id);
                }
                if (startedKind != GameplayActionKind::None)
                {
                    action->current = startedKind;
                }

                action->requested = GameplayActionKind::None;
                action->requestDispatched = false;
            }
        }

        if (detail::NameMatchesAnyAlias_(
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

    inline void WriteGameplayLocomotionAnimationParameters(
        AnimationControllerRuntime& controller,
        const GameplayLocomotionComponent& locomotion)
    {
        detail::SetAnimationFloatParameterByAliases_(
            controller,
            { "MoveX", "InputX", "LocomotionX", "DirectionX", "Strafe", "StrafeX" },
            locomotion.moveX);

        detail::SetAnimationFloatParameterByAliases_(
            controller,
            { "MoveY", "InputY", "LocomotionY", "DirectionY", "Forward", "ForwardY" },
            locomotion.moveY);

        detail::SetAnimationFloatParameterByAliases_(
            controller,
            { "Speed", "MoveSpeed", "PlanarSpeed", "GroundSpeed", "LocomotionSpeed" },
            locomotion.planarSpeed);

        detail::SetAnimationBoolParameterByAliases_(
            controller,
            { "IsMoving", "Moving", "bIsMoving" },
            locomotion.isMoving);

        detail::SetAnimationBoolParameterByAliases_(
            controller,
            { "IsRunning", "Running", "bIsRunning" },
            locomotion.isRunning);
    }

    inline void WriteGameplayActionAnimationParameters(
        AnimationControllerRuntime& controller,
        GameplayActionComponent& action)
    {
        const bool hasRequest = action.requested != GameplayActionKind::None;
        const bool requestAttack = action.requested == GameplayActionKind::LightAttack;
        const bool requestInteract = action.requested == GameplayActionKind::Interact;
        const bool requestJump = action.requested == GameplayActionKind::Jump;
        const bool isAttacking = action.busy && action.current == GameplayActionKind::LightAttack;
        const bool isInteracting = action.busy && action.current == GameplayActionKind::Interact;
        const bool isJumping = action.busy && action.current == GameplayActionKind::Jump;

        detail::SetAnimationBoolParameterByAliases_(
            controller,
            { "ActionRequested", "HasActionRequest", "WantsAction", "bActionRequested" },
            hasRequest);

        detail::SetAnimationBoolParameterByAliases_(
            controller,
            { "ActionBusy", "Busy", "IsBusy", "bBusy" },
            action.busy);

        detail::SetAnimationBoolParameterByAliases_(
            controller,
            { "LightAttackRequested", "AttackRequested", "WantsAttack", "RequestAttack" },
            requestAttack);

        detail::SetAnimationBoolParameterByAliases_(
            controller,
            { "InteractRequested", "UseRequested", "WantsInteract", "RequestInteract" },
            requestInteract);

        detail::SetAnimationBoolParameterByAliases_(
            controller,
            { "JumpRequested", "WantsJump", "RequestJump" },
            requestJump);

        detail::SetAnimationBoolParameterByAliases_(
            controller,
            { "IsAttacking", "Attacking", "bIsAttacking" },
            isAttacking);

        detail::SetAnimationBoolParameterByAliases_(
            controller,
            { "IsInteracting", "Interacting", "bIsInteracting" },
            isInteracting);

        detail::SetAnimationBoolParameterByAliases_(
            controller,
            { "IsJumping", "Jumping", "bIsJumping" },
            isJumping);

        detail::SetAnimationNumericParameterByAliases_(
            controller,
            { "RequestedAction", "RequestedActionKind", "ActionRequestKind", "ActionKindRequested" },
            static_cast<int>(action.requested));

        detail::SetAnimationNumericParameterByAliases_(
            controller,
            { "CurrentAction", "CurrentActionKind", "ActiveAction", "ActionKind" },
            static_cast<int>(action.current));

        if (!action.requestDispatched && hasRequest)
        {
            switch (action.requested)
            {
            case GameplayActionKind::LightAttack:
                detail::FireAnimationTriggerByAliases_(
                    controller,
                    { "LightAttack", "Attack", "LightAttackTrigger", "AttackTrigger" });
                break;
            case GameplayActionKind::Interact:
                detail::FireAnimationTriggerByAliases_(
                    controller,
                    { "Interact", "Use", "InteractTrigger", "UseTrigger" });
                break;
            case GameplayActionKind::Jump:
                detail::FireAnimationTriggerByAliases_(
                    controller,
                    { "Jump", "JumpTrigger", "StartJump" });
                break;
            case GameplayActionKind::None:
            default:
                break;
            }

            action.requestDispatched = true;
        }
    }
}
