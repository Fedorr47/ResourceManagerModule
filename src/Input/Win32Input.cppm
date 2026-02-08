module;

#if defined(_WIN32)
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <windowsx.h> // GET_WHEEL_DELTA_WPARAM
#endif

#include <array>
#include <cstdint>

export module core:win32_input;

import :input;
import :input_core;

export namespace rendern
{
    class Win32Input
    {
    public:
        Win32Input() = default;

        void SetCaptureMode(InputCapture cap) noexcept
        {
            capture_ = cap;
        }

        const InputState& State() const noexcept { return core_.State(); }

#if defined(_WIN32)
        // Feed window messages that carry input info (mouse wheel, focus, etc.).
        void OnWndProc(HWND hwnd, UINT msg, WPARAM wParam, [[maybe_unused]] LPARAM lParam)
        {
            switch (msg)
            {
            case WM_MOUSEWHEEL:
                wheelDeltaUnits_ += GET_WHEEL_DELTA_WPARAM(wParam);
                break;

            case WM_KILLFOCUS:
                ReleaseLook(hwnd);
                break;

            case WM_ACTIVATEAPP:
                // wParam == FALSE when app is deactivated.
                if (wParam == FALSE)
                    ReleaseLook(hwnd);
                break;

            default:
                break;
            }
        }

        // Poll keyboard/mouse and update internal InputState for this frame.
        void NewFrame(HWND hwnd)
        {
            MouseInput mouse{};

            // Focus check
            const bool hasFocus = (GetForegroundWindow() == hwnd);

            // Keyboard: poll all virtual keys.
            std::array<std::uint8_t, 256> curKeyDown{};
            for (int i = 0; i < 256; ++i)
            {
                const bool down = (GetAsyncKeyState(i) & 0x8000) != 0;
                curKeyDown[static_cast<std::uint8_t>(i)] = static_cast<std::uint8_t>(down);
            }

            const bool shiftDown = (curKeyDown[static_cast<std::uint8_t>(VK_SHIFT)] != 0);

            // Mouse buttons
            mouse.rmbDown = (curKeyDown[static_cast<std::uint8_t>(VK_RBUTTON)] != 0);

            // Relative mouse look (hold RMB): managed here (platform layer).
            const bool allowMouse = hasFocus && !capture_.captureMouse;
            if (!allowMouse)
            {
                ReleaseLook(hwnd);
                // Still build a frame (pressed/released), but with zero look delta.
                mouse.lookDx = 0;
                mouse.lookDy = 0;
                const int wheel = TakeWheelDeltaUnits_();
                core_.NewFrame(capture_, hasFocus, curKeyDown, mouse, shiftDown, wheel);
                return;
            }

            if (mouse.rmbDown && !lookActive_)
            {
                BeginLook(hwnd);
            }
            else if (!mouse.rmbDown && lookActive_)
            {
                ReleaseLook(hwnd);
            }

            if (lookActive_)
            {
                UpdateLook(hwnd, mouse.lookDx, mouse.lookDy);
            }

            const int wheel = TakeWheelDeltaUnits_();
            core_.NewFrame(capture_, hasFocus, curKeyDown, mouse, shiftDown, wheel);
        }

#else
        // Stubs for non-Windows builds.
        void OnWndProc(void*, unsigned, std::uintptr_t, std::intptr_t) {}
        void NewFrame(void*) {}
#endif

    private:
#if defined(_WIN32)
        int TakeWheelDeltaUnits_() noexcept
        {
            const int v = wheelDeltaUnits_;
            wheelDeltaUnits_ = 0;
            return v;
        }

        void BeginLook(HWND hwnd)
        {
            lookActive_ = true;

            // Save cursor pos to restore later.
            GetCursorPos(&savedCursorPos_);

            ::SetCapture(hwnd);

            // Hide cursor (ShowCursor uses an internal counter).
            while (ShowCursor(FALSE) >= 0) {}

            CenterCursor(hwnd);
            lastCenterValid_ = true;
        }

        void ReleaseLook([[maybe_unused]] HWND hwnd)
        {
            if (!lookActive_)
                return;

            lookActive_ = false;
            lastCenterValid_ = false;

            ReleaseCapture();

            // Show cursor back.
            while (ShowCursor(TRUE) < 0) {}

            SetCursorPos(savedCursorPos_.x, savedCursorPos_.y);
        }

        void CenterCursor(HWND hwnd)
        {
            RECT rc{};
            GetClientRect(hwnd, &rc);

            POINT pt{};
            pt.x = (rc.left + rc.right) / 2;
            pt.y = (rc.top + rc.bottom) / 2;

            ClientToScreen(hwnd, &pt);
            centerScreen_ = pt;

            SetCursorPos(centerScreen_.x, centerScreen_.y);
        }

        void UpdateLook(HWND hwnd, int& outDx, int& outDy)
        {
            if (!lastCenterValid_)
            {
                CenterCursor(hwnd);
                lastCenterValid_ = true;
                outDx = 0;
                outDy = 0;
                return;
            }

            POINT cur{};
            GetCursorPos(&cur);

            const int dx = cur.x - centerScreen_.x;
            const int dy = cur.y - centerScreen_.y;

            outDx = dx;
            outDy = dy;

            if (dx != 0 || dy != 0)
            {
                // Re-center.
                SetCursorPos(centerScreen_.x, centerScreen_.y);
            }
        }
#endif

    private:
        InputCapture capture_{};
        InputCore core_{};

#if defined(_WIN32)
        int wheelDeltaUnits_{ 0 };

        bool lookActive_{ false };
        bool lastCenterValid_{ false };
        POINT savedCursorPos_{};
        POINT centerScreen_{};
#endif
    };
}
