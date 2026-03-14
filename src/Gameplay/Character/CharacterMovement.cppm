module;

#include <algorithm>
#include <cmath>
#include <vector>

export module core:character_movement;

import :gameplay;
import :math_utils;

export namespace rendern
{
    inline void UpdateGameplayCharacterMovement(
        GameplayWorld& world,
        const std::vector<EntityHandle>& entities,
        const float deltaSeconds)
    {
        const float dt = std::max(deltaSeconds, 0.0f);
        for (const EntityHandle entity : entities)
        {
            GameplayTransformComponent* transform = world.TryGetTransform(entity);
            GameplayCharacterMotorComponent* motor = world.TryGetCharacterMotor(entity);
            GameplayCharacterCommandComponent* command = world.TryGetCharacterCommand(entity);
            GameplayCharacterMovementStateComponent* movementState = world.TryGetCharacterMovementState(entity);
            GameplayActionComponent* action = world.TryGetAction(entity);
            if (transform == nullptr || motor == nullptr || command == nullptr)
            {
                continue;
            }

            const float targetSpeed = command->wantsRun ? motor->maxRunSpeed : motor->maxWalkSpeed;
            const mathUtils::Vec3 targetVelocity = command->moveWorld * (targetSpeed * command->moveMagnitude);
            const mathUtils::Vec3 velocityDelta = targetVelocity - motor->velocity;

            const float currentSpeed = mathUtils::Length(motor->velocity);
            const float desiredSpeed = mathUtils::Length(targetVelocity);
            const float rate = desiredSpeed > currentSpeed ? motor->acceleration : motor->deceleration;
            const float maxDelta = std::max(rate, 0.0f) * dt;
            const float deltaLen = mathUtils::Length(velocityDelta);

            if (deltaLen <= maxDelta || maxDelta <= 1e-6f)
            {
                motor->velocity = targetVelocity;
            }
            else
            {
                motor->velocity = motor->velocity + (velocityDelta * (maxDelta / deltaLen));
            }

            transform->position = transform->position + motor->velocity * dt;

            if (movementState != nullptr)
            {
                movementState->previousFacingYawDegrees = movementState->facingYawDegrees;
                transform->rotationDegrees.y = movementState->desiredFacingYawDegrees;
                movementState->facingYawDegrees = transform->rotationDegrees.y;

                const bool jumping = action != nullptr &&
                    (action->current == GameplayActionKind::Jump || action->requested == GameplayActionKind::Jump);
                movementState->jumping = jumping;
                movementState->grounded = !jumping;
                movementState->falling = false;
            }
        }
    }

    inline void UpdateGameplayCharacterLocomotion(
        GameplayWorld& world,
        const std::vector<EntityHandle>& entities)
    {
        for (const EntityHandle entity : entities)
        {
            const GameplayTransformComponent* transform = world.TryGetTransform(entity);
            const GameplayCharacterMotorComponent* motor = world.TryGetCharacterMotor(entity);
            const GameplayCharacterCommandComponent* command = world.TryGetCharacterCommand(entity);
            GameplayCharacterMovementStateComponent* movementState = world.TryGetCharacterMovementState(entity);
            GameplayLocomotionComponent* locomotion = world.TryGetLocomotion(entity);
            if (transform == nullptr || motor == nullptr || command == nullptr || locomotion == nullptr)
            {
                continue;
            }

            const float planarSpeed = mathUtils::Length(motor->velocity);
            const bool isMoving = planarSpeed > 1e-4f;
            const float yawRadians = mathUtils::DegToRad(transform->rotationDegrees.y);
            const mathUtils::Vec3 actorForward(std::sin(yawRadians), 0.0f, std::cos(yawRadians));
            const mathUtils::Vec3 actorRight(actorForward.z, 0.0f, -actorForward.x);

            locomotion->moveX = command->moveInputX;
            locomotion->moveY = command->moveInputY;
            locomotion->forwardSpeed = mathUtils::Dot(motor->velocity, actorForward);
            locomotion->rightSpeed = mathUtils::Dot(motor->velocity, actorRight);
            locomotion->planarSpeed = planarSpeed;
            locomotion->isMoving = isMoving;
            locomotion->isRunning = isMoving && command->wantsRun;
            locomotion->wantsTurnInPlaceLeft = !isMoving && command->moveInputX < -0.5f;
            locomotion->wantsTurnInPlaceRight = !isMoving && command->moveInputX > 0.5f;

            const float previousYaw = movementState != nullptr
                ? movementState->previousFacingYawDegrees
                : transform->rotationDegrees.y;
            float turnDeltaYawDegrees = transform->rotationDegrees.y - previousYaw;
            while (turnDeltaYawDegrees > 180.0f) turnDeltaYawDegrees -= 360.0f;
            while (turnDeltaYawDegrees < -180.0f) turnDeltaYawDegrees += 360.0f;
            locomotion->turnDeltaYawDegrees = turnDeltaYawDegrees;

            if (movementState != nullptr)
            {
                movementState->previousFacingYawDegrees = transform->rotationDegrees.y;
            }
        }
    }
}
