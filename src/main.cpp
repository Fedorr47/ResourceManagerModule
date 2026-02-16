import core;
import std;

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#if defined(CORE_USE_DX12)
#include <imgui.h>
#include <backends/imgui_impl_win32.h>
// NOTE: Recent Dear ImGui versions intentionally do NOT expose the WndProc handler prototype
// in the header (see comments inside imgui_impl_win32.h). Declare it explicitly.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif

// ------------------------------------------------------------
// Win32 window (no GLFW)
// ------------------------------------------------------------
namespace
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

    // Global pointers used by Win32 WndProc (kept minimal and explicit)
    Win32Window* g_window = nullptr; // main window
    rendern::Win32Input* g_input = nullptr;

#if defined(CORE_USE_DX12)
    Win32Window* g_debugWindow = nullptr;
    bool g_showDebugWindow = true;
    bool g_imguiInitialized = false;
#endif
    // ------------------------------------------------------------
    // Main window menu (UE-style top menu via Win32 menu bar)
    // ------------------------------------------------------------
    constexpr UINT IDM_MAIN_EXIT = 0x1001;
    constexpr UINT IDM_VIEW_DEBUG_WINDOW = 0x2001;

    HMENU g_mainMenu = nullptr;

#if defined(CORE_USE_DX12)
    void UpdateMainMenuDebugWindowCheck()
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

    HMENU CreateMainMenu(bool enableDebugItem, bool debugChecked)
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

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
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

    Win32Window CreateWindowWin32(int width, int height, const std::wstring& title, bool show = true, HMENU menu = nullptr)
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

        // If already registered, RegisterClassExW fails with ERROR_CLASS_ALREADY_EXISTS – that's fine.
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

    void PumpMessages(Win32Window& window)
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

    void TinySleep()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    rhi::Backend ParseBackendFromArgs(int argc, char** argv)
    {
        for (int argIndex = 1; argIndex < argc; ++argIndex)
        {
            const std::string_view argValue = argv[argIndex];
            if (argValue == "--null")
            {
                return rhi::Backend::Null;
            }
        }
        return rhi::Backend::DirectX12;
    }

    void CreateDeviceAndSwapChain(
        rhi::Backend backend,
        HWND hwnd,
        int initialWidth,
        int initialHeight,
        std::unique_ptr<rhi::IRHIDevice>& outDevice,
        std::unique_ptr<rhi::IRHISwapChain>& outSwapChain)
    {
        if (backend == rhi::Backend::DirectX12)
        {
#if defined(CORE_USE_DX12)
            outDevice = rhi::CreateDX12Device();

            rhi::DX12SwapChainDesc swapChainDesc{};
            swapChainDesc.hwnd = hwnd;
            swapChainDesc.bufferCount = 2;
            swapChainDesc.base.extent = rhi::Extent2D{
                static_cast<std::uint32_t>(initialWidth),
                static_cast<std::uint32_t>(initialHeight)
            };
            swapChainDesc.base.backbufferFormat = rhi::Format::BGRA8_UNORM;
            swapChainDesc.base.vsync = false;

            outSwapChain = rhi::CreateDX12SwapChain(*outDevice, swapChainDesc);
#else
            outDevice = rhi::CreateNullDevice();
            rhi::SwapChainDesc swapChainDesc{};
            swapChainDesc.extent = rhi::Extent2D{
                static_cast<std::uint32_t>(initialWidth),
                static_cast<std::uint32_t>(initialHeight)
            };
            outSwapChain = rhi::CreateNullSwapChain(*outDevice, swapChainDesc);
#endif
            return;
        }

        // Null backend
        outDevice = rhi::CreateNullDevice();
        rhi::SwapChainDesc swapChainDesc{};
        swapChainDesc.extent = rhi::Extent2D{
            static_cast<std::uint32_t>(initialWidth),
            static_cast<std::uint32_t>(initialHeight)
        };
        outSwapChain = rhi::CreateNullSwapChain(*outDevice, swapChainDesc);
    }

    static std::unique_ptr<ITextureUploader> CreateTextureUploader(rhi::Backend backend, rhi::IRHIDevice& device)
    {
        switch (backend)
        {
        case rhi::Backend::DirectX12:
#if defined(CORE_USE_DX12)
            return std::make_unique<rendern::DX12TextureUploader>(device);
#else
            return std::make_unique<rendern::NullTextureUploader>(device);
#endif
        default:
            return std::make_unique<rendern::NullTextureUploader>(device);
        }
    }

    // ------------------------------------------------------------
    // App helpers / structs
    // ------------------------------------------------------------
    struct AppConfig
    {
        int windowWidth = 1280;
        int windowHeight = 1024;
        std::wstring windowTitle = L"CoreEngineModule (DX12)";

        // Upload budgets per frame (tune if needed)
        int maxTextureUploadsPerFrame = 8;
        int maxMeshUploadsPerFrame = 32;
        int maxTextureDeletesPerFrame = 2;
        int maxMeshDeletesPerFrame = 32;
    };

    // NOTE: The demo now loads its content from assets/levels/demo.level.json
    // via core:level (LevelAsset + LevelInstance).

#if defined(CORE_USE_DX12)
    void InitializeImGui(HWND hwnd, rhi::IRHIDevice& device, rhi::Format backbufferFormat, int backbufferCount)
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        ImGui_ImplWin32_Init(hwnd);
        device.InitImGui(hwnd, backbufferCount, backbufferFormat);
        g_imguiInitialized = true;

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    }

    void ShutdownImGui(rhi::IRHIDevice& device)
    {
        if (!g_imguiInitialized)
        {
            return;
        }

        device.ShutdownImGui();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_imguiInitialized = false;
    }

    const void* BuildImGuiFrameIfEnabled(rhi::IRHIDevice& device, rendern::RendererSettings& settings, rendern::Scene& scene, rendern::CameraController& cameraController, rendern::LevelAsset& levelAsset, rendern::LevelInstance& levelInstance, AssetManager& assets)
    {
        if (!g_imguiInitialized || !g_showDebugWindow || !g_debugWindow || !g_debugWindow->hwnd)
        {
            return nullptr;
        }

        if (!IsWindowVisible(g_debugWindow->hwnd))
        {
            return nullptr;
        }

        device.ImGuiNewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // style docking host (fills the debug window and enables dock/tabs).
        rendern::ui::BeginDebugDockSpace();

        rendern::ui::DrawRendererDebugUI(settings, scene, cameraController);
        rendern::ui::DrawLevelEditorUI(levelAsset, levelInstance, assets, scene, cameraController);

        ImGui::Render();

        return static_cast<const void*>(ImGui::GetDrawData());
    }

    rendern::InputCapture GetInputCaptureForImGui()
    {
        rendern::InputCapture capture{};
        if (g_imguiInitialized && g_showDebugWindow && g_debugWindow && g_debugWindow->hwnd)
        {
            if (IsWindowVisible(g_debugWindow->hwnd) && GetForegroundWindow() == g_debugWindow->hwnd)
            {
                const ImGuiIO& io = ImGui::GetIO();
                capture.captureKeyboard = io.WantCaptureKeyboard;
                capture.captureMouse = io.WantCaptureMouse;
            }
        }
        return capture;
    }

    void RenderImGuiToSwapChainIfEnabled(rhi::IRHIDevice& device, rhi::IRHISwapChain& swapChain, const void* imguiDrawData)
    {
        if (!imguiDrawData || !g_imguiInitialized || !g_showDebugWindow || !g_debugWindow || !g_debugWindow->hwnd)
        {
            return;
        }
        if (!IsWindowVisible(g_debugWindow->hwnd))
        {
            return;
        }

        const rhi::Extent2D extent = swapChain.GetDesc().extent;

        rhi::CommandList cmd{};

        rhi::BeginPassDesc begin{};
        begin.frameBuffer = swapChain.GetCurrentBackBuffer();
        begin.extent = extent;
        begin.swapChain = &swapChain;
        begin.clearDesc.clearColor = true;
        begin.clearDesc.clearDepth = false;
        begin.clearDesc.color = { 0.08f, 0.08f, 0.08f, 1.0f };

        cmd.BeginPass(begin);
        cmd.SetViewport(0, 0, static_cast<int>(extent.width), static_cast<int>(extent.height));
        cmd.DX12ImGuiRender(imguiDrawData);
        cmd.EndPass();

        device.SubmitCommandList(std::move(cmd));
        swapChain.Present();
    }
#else
    const void* BuildImGuiFrameIfEnabled(rhi::IRHIDevice&, rendern::RendererSettings&, rendern::Scene&, rendern::CameraController&, rendern::LevelAsset&, rendern::LevelInstance&, AssetManager&)
    {
        return nullptr;
    }
    rendern::InputCapture GetInputCaptureForImGui()
    {
        return {};
    }
#endif

    struct SceneHandles
    {
        rendern::MeshHandle cubeMesh{};
        rendern::MeshHandle groundMesh{};
        rendern::MeshHandle quadMesh{};

        rendern::MaterialHandle groundMaterial{};
        rendern::MaterialHandle cubeMaterial{};
        rendern::MaterialHandle glassMaterial{};
    };

    // Old hardcoded demo scene builders are kept in this file for reference, but the
    // main loop below uses LevelAsset/LevelInstance instead.

    void ConfigureDefaultCamera(rendern::Scene& scene)
    {
        scene.camera.position = { 5.0f, 10.0f, 10.0f };
        scene.camera.target = { 0.0f, 0.0f, 0.0f };
        scene.camera.up = { 0.0f, 1.0f, 0.0f };
        scene.camera.fovYDeg = 60.0f;
        scene.camera.nearZ = 0.01f;
        scene.camera.farZ = 200.0f;
    }

    void AddDefaultLights(rendern::Scene& scene)
    {
        // Directional
        {
            rendern::Light light{};
            light.type = rendern::LightType::Directional;
            light.direction = mathUtils::Normalize(mathUtils::Vec3(-0.4f, -1.0f, -0.3f)); // FROM light
            light.color = { 1.0f, 0.2f, 1.0f };
            light.intensity = 0.2f;
            scene.AddLight(light);
        }

        // Point
        {
            rendern::Light light{};
            light.type = rendern::LightType::Point;
            light.position = { -1.35f, 7.0f, -1.35f };
            light.color = { 0.2f, 1.0f, 0.2f };
            light.range = 120.0f;
            light.intensity = 0.1f;
            light.attConstant = 1.0f;
            light.attLinear = 0.02f;
            light.attQuadratic = 0.004f;
            scene.AddLight(light);
        }

        // Spot
        {
            rendern::Light light{};
            light.type = rendern::LightType::Spot;
            light.position = { 2.0f, 4.0f, 2.0f };
            light.direction = mathUtils::Normalize(mathUtils::Vec3(-2.0f, -5.0f, 0.0f)); // FROM light
            light.color = { 0.2f, 0.2f, 1.0f };
            light.range = 100.0f;
            light.intensity = 8.0f;
            light.innerHalfAngleDeg = 22.0f;
            light.outerHalfAngleDeg = 35.0f;
            light.attLinear = 0.09f;
            light.attQuadratic = 0.032f;
            scene.AddLight(light);
        }
    }

    SceneHandles CreateDefaultMaterials(rendern::Scene& scene)
    {
        SceneHandles handles{};

        // Ground material (no texture)
        rendern::Material groundMaterial{};
        groundMaterial.params.baseColor = { 0.8f, 0.8f, 0.8f, 1.0f };
        groundMaterial.params.albedoDescIndex = 0;
        groundMaterial.params.shininess = 16.0f;
        groundMaterial.params.specStrength = 0.5f;
        groundMaterial.params.shadowBias = 0.0f;
        groundMaterial.params.metallic = 0.0f;
        groundMaterial.params.roughness = 0.9f;
        groundMaterial.params.ao = 1.0f;
        groundMaterial.params.emissiveStrength = 1.0f;
        groundMaterial.permFlags = rendern::MaterialPerm::UseShadow;

        // Cube material (texture desc will be assigned once available)
        rendern::Material cubeMaterial{};
        cubeMaterial.params.baseColor = { 1.0f, 1.0f, 1.0f, 1.0f };
        cubeMaterial.params.albedoDescIndex = 0;
        cubeMaterial.params.shininess = 64.0f;
        cubeMaterial.params.specStrength = 0.5f;
        cubeMaterial.params.shadowBias = 0.0015f;
        cubeMaterial.params.metallic = 0.0f;
        cubeMaterial.params.roughness = 0.75f;
        cubeMaterial.params.ao = 1.0f;
        cubeMaterial.params.emissiveStrength = 1.0f;
        cubeMaterial.permFlags = rendern::MaterialPerm::UseShadow;

        // Glass material (transparent)
        rendern::Material glassMaterial{};
        glassMaterial.params.baseColor = { 0.2f, 0.6f, 1.0f, 0.35f };
        glassMaterial.params.albedoDescIndex = 0;
        glassMaterial.params.shininess = 128.0f;
        glassMaterial.params.specStrength = 0.9f;
        glassMaterial.params.shadowBias = 0.0f;
        glassMaterial.params.metallic = 0.0f;
        glassMaterial.params.roughness = 0.08f;
        glassMaterial.params.ao = 1.0f;
        glassMaterial.params.emissiveStrength = 1.0f;
        glassMaterial.permFlags = rendern::MaterialPerm::UseShadow | rendern::MaterialPerm::Transparent;

        handles.groundMaterial = scene.CreateMaterial(groundMaterial);
        handles.cubeMaterial = scene.CreateMaterial(cubeMaterial);
        handles.glassMaterial = scene.CreateMaterial(glassMaterial);

        return handles;
    }

    void AddGround(rendern::Scene& scene, const rendern::MeshHandle groundMesh, const rendern::MaterialHandle groundMaterial)
    {
        rendern::DrawItem groundItem{};
        groundItem.mesh = groundMesh;
        groundItem.transform.position = { 0.0f, -0.6f, 0.0f };
        groundItem.transform.rotationDegrees = { -90.0f, 0.0f, 0.0f }; // quad XY -> XZ
        groundItem.transform.scale = { 8.0f, 8.0f, 8.0f };
        groundItem.material = groundMaterial;
        scene.AddDraw(groundItem);
    }

    void AddGlassPane(rendern::Scene& scene, const rendern::MeshHandle quadMesh, const rendern::MaterialHandle glassMaterial)
    {
        rendern::DrawItem glassItem{};
        glassItem.mesh = quadMesh; // quad.obj (XY plane)
        glassItem.transform.position = { 0.0f, 2.3f, 2.6f };
        glassItem.transform.rotationDegrees = { 0.0f, 0.0f, 0.0f };
        glassItem.transform.scale = { 4.0f, 4.0f, 4.0f };
        glassItem.material = glassMaterial;
        scene.AddDraw(glassItem);
    }

    void AddCubeGrid(rendern::Scene& scene, const rendern::MeshHandle cubeMesh, const rendern::MaterialHandle cubeMaterial)
    {
        constexpr int gridDim = 10;
        constexpr float spacing = 1.35f;

        for (int gridZ = 0; gridZ < gridDim; ++gridZ)
        {
            for (int gridX = 0; gridX < gridDim; ++gridX)
            {
                const float posX = (gridX - (gridDim / 2)) * spacing;
                const float posZ = (gridZ - (gridDim / 2)) * spacing;

                rendern::DrawItem cubeItem{};
                cubeItem.mesh = cubeMesh;
                cubeItem.transform.position = { posX, 2.3f, posZ };
                cubeItem.transform.rotationDegrees = { 0.0f, 0.0f, 0.0f };
                cubeItem.transform.scale = { 1.0f, 1.0f, 1.0f };
                cubeItem.material = cubeMaterial;
                scene.AddDraw(cubeItem);
            }
        }
    }

    // Descriptor management moved to rendern::LevelInstance.

} // namespace

int main(int argc, char** argv)
{
    try
    {
        const AppConfig config{};
        const rhi::Backend requestedBackend = ParseBackendFromArgs(argc, argv);

        const bool canUseDebugWindow =
#if defined(CORE_USE_DX12)
            (requestedBackend == rhi::Backend::DirectX12);
#else
            false;
#endif

#if defined(CORE_USE_DX12)
        g_showDebugWindow = canUseDebugWindow;
#endif

        g_mainMenu = CreateMainMenu(canUseDebugWindow, canUseDebugWindow);
        Win32Window window = CreateWindowWin32(config.windowWidth, config.windowHeight, config.windowTitle, /*show=*/true, g_mainMenu);
        g_window = &window;

#if defined(CORE_USE_DX12)
        Win32Window debugWindow{};
        std::unique_ptr<rhi::IRHISwapChain> debugSwapChain;
        if (requestedBackend == rhi::Backend::DirectX12)
        {
            debugWindow = CreateWindowWin32(900, 900, L"CoreEngineModule - Debug UI", /*show=*/g_showDebugWindow);
            g_debugWindow = &debugWindow;
            UpdateMainMenuDebugWindowCheck();
        }
#endif

        rendern::Win32Input win32Input{};
        g_input = &win32Input;

        std::unique_ptr<rhi::IRHIDevice> device;
        std::unique_ptr<rhi::IRHISwapChain> swapChain;
        CreateDeviceAndSwapChain(requestedBackend, window.hwnd, config.windowWidth, config.windowHeight, device, swapChain);

#if defined(CORE_USE_DX12)
        if (requestedBackend == rhi::Backend::DirectX12)
        {
            rhi::DX12SwapChainDesc debugSwapChainDesc{};
            debugSwapChainDesc.hwnd = debugWindow.hwnd;
            debugSwapChainDesc.bufferCount = 2;
            debugSwapChainDesc.base.extent = rhi::Extent2D{ 900u, 900u };
            debugSwapChainDesc.base.backbufferFormat = rhi::Format::BGRA8_UNORM;
            debugSwapChainDesc.base.vsync = false;

            debugSwapChain = rhi::CreateDX12SwapChain(*device, debugSwapChainDesc);
        }
#endif

        // Asset/Resource system: CPU decode on job system, GPU upload on render queue.
        StbTextureDecoder textureDecoder{};
        rendern::JobSystemThreadPool jobSystem{ 1 };
        rendern::RenderQueueImmediate renderQueue{};
        std::unique_ptr<ITextureUploader> textureUploader = CreateTextureUploader(device->GetBackend(), *device);

        TextureIO textureIO{ textureDecoder, *textureUploader, jobSystem, renderQueue };
        rendern::MeshIO meshIO{ *device, jobSystem, renderQueue };

        AssetManager assets{ textureIO, meshIO };
        ResourceManager& resourceManager = assets.GetResourceManager();

        // Level asset (JSON)
        rendern::LevelAsset levelAsset = rendern::LoadLevelAssetFromJson("levels/demo.level.json");

        // Renderer (facade) - Stage1 expects Scene
        rendern::RendererSettings rendererSettings{};
        rendererSettings.drawLightGizmos = true;
        rendern::Renderer renderer{ *device, rendererSettings };

#if defined(CORE_USE_DX12)
        if (requestedBackend == rhi::Backend::DirectX12 && debugSwapChain && debugWindow.hwnd)
        {
            InitializeImGui(debugWindow.hwnd, *device, debugSwapChain->GetDesc().backbufferFormat, /*backbufferCount=*/2);
        }
#endif

        // Scene
        rendern::Scene scene{};
        scene.Clear();

        // Level instantiation requests meshes/textures and fills Scene (draws/materials/lights/camera).
        rendern::BindlessTable bindless{ *device };
        rendern::LevelInstance levelInstance = rendern::InstantiateLevel(
            scene,
            assets,
            bindless,
            levelAsset,
            mathUtils::Mat4(1.0f));

        rendern::CameraController cameraController{};
        cameraController.ResetFromCamera(scene.camera);

        // Timer
        GameTimer frameTimer{};
        frameTimer.SetMaxDelta(0.05);
        frameTimer.Reset();

        while (window.running)
        {
            PumpMessages(window);
            if (!window.running)
            {
                break;
            }

            // Apply pending window resizes (recreate swapchain buffers)
            if (swapChain && window.pendingResize)
            {
                window.pendingResize = false;
                if (window.pendingWidth > 0 && window.pendingHeight > 0)
                {
                    swapChain->Resize(rhi::Extent2D{
                        static_cast<std::uint32_t>(window.pendingWidth),
                        static_cast<std::uint32_t>(window.pendingHeight)
                    });
                }
            }

#if defined(CORE_USE_DX12)
            if (debugSwapChain && debugWindow.hwnd && debugWindow.pendingResize)
            {
                debugWindow.pendingResize = false;
                if (debugWindow.pendingWidth > 0 && debugWindow.pendingHeight > 0)
                {
                    debugSwapChain->Resize(rhi::Extent2D{
                        static_cast<std::uint32_t>(debugWindow.pendingWidth),
                        static_cast<std::uint32_t>(debugWindow.pendingHeight)
                    });
                }
            }
#endif

            // If main window is minimized, skip rendering/presenting to avoid DXGI issues.
            if (window.minimized || window.width <= 0 || window.height <= 0)
            {
                TinySleep();
                continue;
            }

            // Drive uploads/destruction
            assets.ProcessUploads(
                config.maxTextureUploadsPerFrame,
                config.maxTextureDeletesPerFrame,
                config.maxMeshUploadsPerFrame,
                config.maxMeshDeletesPerFrame);

            // As GPU textures become available, allocate/update descriptor indices.
            levelInstance.ResolveTextureBindings(assets, bindless, scene);

            // Delta time
            frameTimer.Tick();
            const float deltaSeconds = static_cast<float>(frameTimer.GetDeltaTime());

            // Input + camera controller
            win32Input.SetCaptureMode(GetInputCaptureForImGui());
            win32Input.NewFrame(window.hwnd);
            cameraController.Update(deltaSeconds, win32Input.State(), scene.camera);

            // Keep draw item transforms in sync even when the debug UI is closed.
            levelInstance.SyncTransformsIfDirty(levelAsset, scene);

            // Mouse picking in MAIN viewport (LMB selects a node).
            {
                const rendern::InputState& in = win32Input.State();
                if (in.hasFocus && in.KeyPressed(VK_LBUTTON) && !in.mouse.rmbDown && !in.capture.captureMouse)
                {
                    POINT pt{};
                    if (GetCursorPos(&pt) && ScreenToClient(window.hwnd, &pt))
                    {
                        const int mx = pt.x;
                        const int my = pt.y;

                        if (mx >= 0 && my >= 0 && mx < window.width && my < window.height)
                        {
                            const rendern::PickResult pick = rendern::PickNodeUnderScreenPoint(
                                scene,
                                levelInstance,
                                static_cast<float>(mx),
                                static_cast<float>(my),
                                static_cast<float>(window.width),
                                static_cast<float>(window.height));

                            scene.debugPickRay.enabled = true;
                            scene.debugPickRay.origin = pick.rayOrigin;
                            scene.debugPickRay.direction = pick.rayDir;
                            scene.debugPickRay.hit = (pick.nodeIndex >= 0) && std::isfinite(pick.t);
                            scene.debugPickRay.length = scene.debugPickRay.hit ? pick.t : scene.camera.farZ;

                            if (scene.debugPickRay.hit && levelInstance.IsNodeAlive(levelAsset, pick.nodeIndex))
                            {
                                scene.editorSelectedNode = pick.nodeIndex;
                            }
                            else
                            {
                                scene.editorSelectedNode = -1;
                            }
                        }
                    }
                }
            }

            // ImGui (optional) - rendered into a separate debug window swapchain
            const void* imguiDrawData = BuildImGuiFrameIfEnabled(*device, rendererSettings, scene, cameraController, levelAsset, levelInstance, assets);

            // Render main scene (no UI overlay)
            renderer.SetSettings(rendererSettings);
            renderer.RenderFrame(*swapChain, scene, /*imguiDrawData=*/nullptr);

#if defined(CORE_USE_DX12)
            if (debugSwapChain && debugWindow.hwnd && !debugWindow.minimized && debugWindow.width > 0 && debugWindow.height > 0)
            {
                RenderImGuiToSwapChainIfEnabled(*device, *debugSwapChain, imguiDrawData);
            }
#endif

            TinySleep();
        }

#if defined(CORE_USE_DX12)
        ShutdownImGui(*device);
#endif

        renderer.Shutdown();

        // Descriptors cleanup
        levelInstance.FreeDescriptors(bindless);

        // Cleanup resources (destroy queues are driven by ProcessUploads).
        jobSystem.WaitIdle();
        assets.ClearAll();
        assets.ProcessUploads(64, 256, 64, 256);

        if (window.hwnd)
        {
            DestroyWindow(window.hwnd);
            window.hwnd = nullptr;
        }

#if defined(CORE_USE_DX12)
        if (debugWindow.hwnd)
        {
            DestroyWindow(debugWindow.hwnd);
            debugWindow.hwnd = nullptr;
        }
        g_debugWindow = nullptr;
#endif

        g_window = nullptr;
        g_input = nullptr;

        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Fatal: " << exception.what() << "\n";
        return 2;
    }
}
