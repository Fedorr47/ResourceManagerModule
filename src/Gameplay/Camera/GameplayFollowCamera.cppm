module;

#include <algorithm>
#include <cmath>

export module core:gameplay_follow_camera;

import :gameplay;
import :gameplay_runtime_common;
import :math_utils;
import :scene;

export namespace rendern
{
    class GameplayFollowCameraController final
    {
    public:
        void Reset(GameplayWorld& world, const EntityHandle entity) const noexcept
        {
            if (GameplayFollowCameraComponent* camera = world.TryGetFollowCamera(entity))
            {
                camera->initialized = false;
            }
        }

        bool Update(GameplayWorld& world, const EntityHandle entity, const GameplayUpdateContext& ctx) const noexcept
        {
            if (ctx.scene == nullptr)
            {
                return false;
            }

            GameplayTransformComponent* transform = world.TryGetTransform(entity);
            GameplayFollowCameraComponent* followCamera = world.TryGetFollowCamera(entity);
            if (transform == nullptr || followCamera == nullptr)
            {
                return false;
            }

            Camera& sceneCamera = ctx.scene->camera;
            if (!followCamera->initialized)
            {
                SyncFromSceneCamera_(*followCamera, sceneCamera);
                followCamera->initialized = true;
            }

            if (followCamera->consumeMouseLook &&
                ctx.input != nullptr &&
                ctx.input->hasFocus &&
                !ctx.input->capture.captureMouse)
            {
                followCamera->yawRad -= static_cast<float>(ctx.input->mouse.lookDx) * followCamera->mouseSensitivity;
                followCamera->pitchRad -= static_cast<float>(ctx.input->mouse.lookDy) * followCamera->mouseSensitivity;
            }

            followCamera->pitchRad = std::clamp(
                followCamera->pitchRad,
                -followCamera->maxPitchRad,
                followCamera->maxPitchRad);
            followCamera->yawRad = std::remainder(followCamera->yawRad, mathUtils::TwoPi);

            const mathUtils::Vec3 focusTarget = transform->position + followCamera->focusOffset;
            const float cy = std::cos(followCamera->yawRad);
            const float sy = std::sin(followCamera->yawRad);
            const float cp = std::cos(followCamera->pitchRad);
            const float sp = std::sin(followCamera->pitchRad);
            const mathUtils::Vec3 forward = mathUtils::Normalize(mathUtils::Vec3(sy * cp, sp, cy * cp));

            sceneCamera.position = focusTarget - forward * followCamera->distance;
            sceneCamera.target = focusTarget;
            sceneCamera.up = mathUtils::Vec3(0.0f, 1.0f, 0.0f);
            return true;
        }

    private:
        static void SyncFromSceneCamera_(GameplayFollowCameraComponent& followCamera, const Camera& sceneCamera) noexcept
        {
            mathUtils::Vec3 initialForward = sceneCamera.target - sceneCamera.position;
            if (mathUtils::Length(initialForward) <= 1e-6f)
            {
                initialForward = mathUtils::Vec3(0.0f, 0.0f, 1.0f);
            }
            else
            {
                initialForward = mathUtils::Normalize(initialForward);
            }

            followCamera.yawRad = std::atan2(initialForward.x, initialForward.z);
            followCamera.pitchRad = std::asin(std::clamp(initialForward.y, -1.0f, 1.0f));
        }
    };
}
