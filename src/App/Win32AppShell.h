#pragma once

#if defined(CORE_USE_DX12)
#include <backends/imgui_impl_win32.h>
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif

namespace appWin32
{
    struct Win32Window
    {
        HWND hwnd{};
        int width{};
        int height{};
        bool pendingResize{ false };
        int pendingWidth{};
        int pendingHeight{};
        bool minimized{ false };
        bool running{ true };
    };

    inline Win32Window* g_window = nullptr; // main window
    inline rendern::Win32Input* g_input = nullptr;

#if defined(CORE_USE_DX12)
    inline Win32Window* g_debugWindow = nullptr;
    inline bool g_showDebugWindow = true;
    inline bool g_imguiInitialized = false;
#endif

    constexpr UINT IDM_MAIN_EXIT = 0x1001;
    constexpr UINT IDM_VIEW_DEBUG_WINDOW = 0x2001;

    inline HMENU g_mainMenu = nullptr;

#if defined(CORE_USE_DX12)
    inline void UpdateMainMenuDebugWindowCheck()
    {
        if (!g_mainMenu)
        {
            return;
        }

        const UINT enableFlags = MF_BYCOMMAND | ((g_debugWindow && g_debugWindow->hwnd) ? MF_ENABLED : MF_GRAYED);
        EnableMenuItem(g_mainMenu, IDM_VIEW_DEBUG_WINDOW, enableFlags);

        const UINT checkFlags = MF_BYCOMMAND | (g_showDebugWindow ? MF_CHECKED : MF_UNCHECKED);
        CheckMenuItem(g_mainMenu, IDM_VIEW_DEBUG_WINDOW, checkFlags);

        if (g_window && g_window->hwnd)
        {
            DrawMenuBar(g_window->hwnd);
        }
    }
#endif

    inline HMENU CreateMainMenu(bool enableDebugItem, bool debugChecked)
    {
        HMENU menu = CreateMenu();
        HMENU mainPopup = CreatePopupMenu();
        HMENU viewPopup = CreatePopupMenu();

        AppendMenuW(mainPopup, MF_STRING, IDM_MAIN_EXIT, L"Exit");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(mainPopup), L"Main");

        UINT viewFlags = MF_STRING;
        if (!enableDebugItem)
        {
            viewFlags |= MF_GRAYED;
        }
        if (debugChecked)
        {
            viewFlags |= MF_CHECKED;
        }

        AppendMenuW(viewPopup, viewFlags, IDM_VIEW_DEBUG_WINDOW, L"Open Debug Window\tF1");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(viewPopup), L"View");

        return menu;
    }

    inline LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (g_input && g_window && hwnd == g_window->hwnd)
        {
            g_input->OnWndProc(hwnd, msg, wParam, lParam);
        }

#if defined(CORE_USE_DX12)
        if (g_imguiInitialized && g_debugWindow && hwnd == g_debugWindow->hwnd)
        {
            if (msg != WM_SIZE && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
            {
                return 1;
            }
        }
#endif

        switch (msg)
        {
        case WM_COMMAND:
        {
            const int cmdId = static_cast<int>(LOWORD(wParam));
            if (g_window && hwnd == g_window->hwnd)
            {
                switch (cmdId)
                {
                case IDM_MAIN_EXIT:
                    DestroyWindow(hwnd);
                    return 0;

#if defined(CORE_USE_DX12)
                case IDM_VIEW_DEBUG_WINDOW:
                    g_showDebugWindow = !g_showDebugWindow;
                    if (g_debugWindow && g_debugWindow->hwnd)
                    {
                        ShowWindow(g_debugWindow->hwnd, g_showDebugWindow ? SW_SHOW : SW_HIDE);
                        if (g_showDebugWindow)
                        {
                            SetForegroundWindow(g_debugWindow->hwnd);
                        }
                    }
                    UpdateMainMenuDebugWindowCheck();
                    return 0;
#endif

                default:
                    break;
                }
            }
            break;
        }
        case WM_CLOSE:
#if defined(CORE_USE_DX12)
            if (g_debugWindow && hwnd == g_debugWindow->hwnd)
            {
                ShowWindow(hwnd, SW_HIDE);
                g_showDebugWindow = false;
                UpdateMainMenuDebugWindowCheck();
                return 0;
            }
#endif

            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (g_window && hwnd == g_window->hwnd)
            {
                g_window->running = false;
                PostQuitMessage(0);
            }
            return 0;

        case WM_SIZE:
        {
            Win32Window* win = nullptr;
            if (g_window && hwnd == g_window->hwnd)
            {
                win = g_window;
            }
#if defined(CORE_USE_DX12)
            else if (g_debugWindow && hwnd == g_debugWindow->hwnd)
            {
                win = g_debugWindow;
            }
#endif
            if (win)
            {
                const int newW = static_cast<int>(LOWORD(lParam));
                const int newH = static_cast<int>(HIWORD(lParam));
                win->width = newW;
                win->height = newH;
                win->pendingWidth = newW;
                win->pendingHeight = newH;
                win->pendingResize = true;
                win->minimized = (wParam == SIZE_MINIMIZED) || (newW == 0) || (newH == 0);
                return 0;
            }
            break;
        }

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE)
            {
                if (g_window && hwnd == g_window->hwnd)
                {
                    DestroyWindow(hwnd);
                    return 0;
                }
            }
#if defined(CORE_USE_DX12)
            if (wParam == VK_F1)
            {
                const bool wasDown = (lParam & (1 << 30)) != 0;
                if (!wasDown)
                {
                    g_showDebugWindow = !g_showDebugWindow;
                    if (g_debugWindow && g_debugWindow->hwnd)
                    {
                        ShowWindow(g_debugWindow->hwnd, g_showDebugWindow ? SW_SHOW : SW_HIDE);
                        if (g_showDebugWindow)
                        {
                            SetForegroundWindow(g_debugWindow->hwnd);
                        }
                    }
                    UpdateMainMenuDebugWindowCheck();
                }
                return 0;
            }
#endif
            break;

        default:
            break;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    inline Win32Window CreateWindowWin32(int width, int height, const std::wstring& title, bool show = true, HMENU menu = nullptr)
    {
        Win32Window window{};
        window.width = width;
        window.height = height;

        const HINSTANCE instanceHandle = GetModuleHandleW(nullptr);
        const wchar_t* className = L"CoreEngineModuleWindowClass";

        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = WndProc;
        windowClass.hInstance = instanceHandle;
        windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        windowClass.lpszClassName = className;

        if (!RegisterClassExW(&windowClass))
        {
            const DWORD errorCode = GetLastError();
            if (errorCode != ERROR_CLASS_ALREADY_EXISTS)
            {
                throw std::runtime_error("RegisterClassExW failed");
            }
        }

        const DWORD style = WS_OVERLAPPEDWINDOW;

        RECT rect{ 0, 0, width, height };
        AdjustWindowRect(&rect, style, menu != nullptr);

        window.hwnd = CreateWindowExW(
            0,
            className,
            title.c_str(),
            style,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            rect.right - rect.left,
            rect.bottom - rect.top,
            nullptr,
            menu,
            instanceHandle,
            nullptr);

        if (!window.hwnd)
        {
            throw std::runtime_error("CreateWindowExW failed");
        }

        ShowWindow(window.hwnd, show ? SW_SHOW : SW_HIDE);
        UpdateWindow(window.hwnd);
        return window;
    }

    inline void PumpMessages(Win32Window& window)
    {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                window.running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    inline void TinySleep()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
} // namespace appWin32
