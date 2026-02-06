module;

#include <cmath>

export module core:camera_controller;

import std;

import :controller_base;
import :scene;
import :math_utils;
import :input;

export namespace rendern
{
    struct CameraControllerSettings
    {
        float moveSpeed{ 8.0f };            // units per second
        float sprintMultiplier{ 4.0f };     // when shiftDown
        float mouseSensitivity{ 0.0025f };  // radians per pixel
        bool invertY{ false };
    };

    // Fly/free-look camera (platform-agnostic):
    //  - Uses InputState provided by the platform input layer.
    //  - Movement: WASD + QE, Shift to sprint.
    //  - Keeps Camera::target = position + forward.
    class CameraController final : public ControllerBase
    {
    public:
        CameraController() = default;

        CameraControllerSettings& Settings() noexcept { return settings_; }
        const CameraControllerSettings& Settings() const noexcept { return settings_; }

        // Initialize yaw/pitch from the current camera vectors.
        void ResetFromCamera(const Camera& cam)
        {
            mathUtils::Vec3 f = cam.target - cam.position;
            const float len = mathUtils::Length(f);
            if (len < 1e-6f)
            {
                yawRad_ = 0.0f;
                pitchRad_ = 0.0f;
                return;
            }

            f = f * (1.0f / len);

            // Convention: yaw around +Y, pitch around +X (looking up/down).
            // Forward reconstruction:
            //  f.x = sin(yaw) * cos(pitch)
            //  f.y = sin(pitch)
            //  f.z = cos(yaw) * cos(pitch)
            yawRad_ = std::atan2(f.x, f.z);

            const float y = std::clamp(f.y, -1.0f, 1.0f);
            pitchRad_ = std::asin(y);
            ClampPitch();
        }

        float YawRad() const noexcept { return yawRad_; }
        float PitchRad() const noexcept { return pitchRad_; }

        void SetYawPitchRad(float yaw, float pitch, Camera& cam)
        {
            yawRad_ = yaw;
            pitchRad_ = pitch;
            ClampPitch();
            ApplyToCamera(cam);
        }

        mathUtils::Vec3 Forward() const
        {
            const float cy = std::cos(yawRad_);
            const float sy = std::sin(yawRad_);
            const float cp = std::cos(pitchRad_);
            const float sp = std::sin(pitchRad_);
            return mathUtils::Normalize(mathUtils::Vec3(sy * cp, sp, cy * cp));
        }

        void Update(float dt, const InputState& input, Camera& cam)
        {
            // Even if gated off, keep target consistent with current orientation.
            if (!CanUpdate(input))
            {
                ApplyToCamera(cam);
                return;
            }

            if (!input.hasFocus)
            {
                ApplyToCamera(cam);
                return;
            }

            const bool allowKeyboard = !input.capture.captureKeyboard;
            const bool allowMouse = !input.capture.captureMouse;

            if (allowMouse)
            {
                UpdateLook(input);
                UpdateSpeedFromWheel(input);
            }

            if (AllowKeyboard(input))
            {
                UpdateMove(dt, input, cam);
            }

            ApplyToCamera(cam);
        }

    private:
        void ClampPitch() noexcept
        {
            // Prevent gimbal singularity (looking straight up/down).
            constexpr float kMaxPitch = mathUtils::DegToRad(89.0f); // ~89 degrees
            pitchRad_ = std::clamp(pitchRad_, -kMaxPitch, kMaxPitch);
        }

        void NormalizeYaw()
        {
            yawRad_ = std::remainder(yawRad_, mathUtils::TwoPi);
        }

        void ApplyToCamera(Camera& cam)
        {
            cam.up = mathUtils::Vec3(0.0f, 1.0f, 0.0f);
            cam.target = cam.position + Forward();
        }

        void UpdateLook(const InputState& input)
        {
            const int deltaX = input.mouse.lookDx;
            const int deltaY = input.mouse.lookDy;

            if (deltaX == 0 && deltaY == 0)
            {
                return;
            }

            yawRad_ -= static_cast<float>(deltaX) * settings_.mouseSensitivity;

            const float invertSign = settings_.invertY ? 1.0f : -1.0f;
            pitchRad_ += invertSign * static_cast<float>(deltaY) * settings_.mouseSensitivity;

            NormalizeYaw();
            ClampPitch();
        }

        void UpdateSpeedFromWheel(const InputState& input)
        {
            const int w = input.mouse.wheelSteps;
            if (w == 0)
                return;

            // Exponential feels nicer than linear.
            const float factor = std::pow(1.1f, static_cast<float>(w));
            settings_.moveSpeed = std::clamp(settings_.moveSpeed * factor, 0.1f, 50.0f);
        }

        void UpdateMove(float dt, const InputState& input, Camera& cam)
        {
            const float base = settings_.moveSpeed;
            const float mul = input.shiftDown ? settings_.sprintMultiplier : 1.0f;
            const float speed = base * mul;

            const mathUtils::Vec3 worldUp(0.0f, 1.0f, 0.0f);

            mathUtils::Vec3 f = Forward();
            mathUtils::Vec3 fFlat{ f.x, 0.0f, f.z };
            const float fLen = mathUtils::Length(fFlat);
            if (fLen > 1e-6f)
                fFlat = fFlat * (1.0f / fLen);
            else
                fFlat = mathUtils::Vec3(0.0f, 0.0f, 1.0f);

            mathUtils::Vec3 right = mathUtils::Normalize(mathUtils::Cross(worldUp, fFlat));

            mathUtils::Vec3 move(0.0f, 0.0f, 0.0f);

            if (input.KeyDown('W')) move = move + fFlat;
            if (input.KeyDown('S')) move = move - fFlat;
            if (input.KeyDown('D')) move = move - right;
            if (input.KeyDown('A')) move = move + right;
            if (input.KeyDown('E')) move = move + worldUp;
            if (input.KeyDown('Q')) move = move - worldUp;

            const float mLen = mathUtils::Length(move);
            if (mLen > 1e-6f)
                move = move * (1.0f / mLen);

            cam.position = cam.position + move * (speed * dt);
        }

        CameraControllerSettings settings_{};

        float yawRad_{ 0.0f };
        float pitchRad_{ 0.0f };
    };
}
