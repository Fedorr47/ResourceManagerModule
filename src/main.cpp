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
        bool running{ true };
    };

    // Global pointers used by Win32 WndProc (kept minimal and explicit)
    Win32Window* g_window = nullptr;
    rendern::Win32Input* g_input = nullptr;

#if defined(CORE_USE_DX12)
    bool g_showUI = true;
    bool g_imguiInitialized = false;
#endif

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (g_input)
        {
            g_input->OnWndProc(hwnd, msg, wParam, lParam);
        }

#if defined(CORE_USE_DX12)
        if (g_imguiInitialized)
        {
            if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
            {
                return 1;
            }
        }
#endif

        switch (msg)
        {
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (g_window)
            {
                g_window->running = false;
            }
            PostQuitMessage(0);
            return 0;

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE)
            {
                DestroyWindow(hwnd);
                return 0;
            }
#if defined(CORE_USE_DX12)
            if (wParam == VK_F1)
            {
                const bool wasDown = (lParam & (1 << 30)) != 0;
                if (!wasDown)
                {
                    g_showUI = !g_showUI;
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

    Win32Window CreateWindowWin32(int width, int height, const std::wstring& title)
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

        // Disable resizing for now (swapchain resize handling isn't wired here).
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

        RECT rect{ 0, 0, width, height };
        AdjustWindowRect(&rect, style, FALSE);

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
            nullptr,
            instanceHandle,
            nullptr);

        if (!window.hwnd)
        {
            throw std::runtime_error("CreateWindowExW failed");
        }

        ShowWindow(window.hwnd, SW_SHOW);
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
        if (!g_imguiInitialized)
        {
            return nullptr;
        }

        device.ImGuiNewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (g_showUI)
        {
            rendern::ui::DrawRendererDebugUI(settings, scene, cameraController);
            rendern::ui::DrawLevelEditorUI(levelAsset, levelInstance, assets, scene, cameraController);
        }

        ImGui::Render();

        if (g_showUI)
        {
            return static_cast<const void*>(ImGui::GetDrawData());
        }

        return nullptr;
    }

    rendern::InputCapture GetInputCaptureForImGui()
    {
        rendern::InputCapture capture{};
        if (g_imguiInitialized && g_showUI)
        {
            const ImGuiIO& io = ImGui::GetIO();
            capture.captureKeyboard = io.WantCaptureKeyboard;
            capture.captureMouse = io.WantCaptureMouse;
        }
        return capture;
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

        Win32Window window = CreateWindowWin32(config.windowWidth, config.windowHeight, config.windowTitle);
        g_window = &window;

        rendern::Win32Input win32Input{};
        g_input = &win32Input;

        std::unique_ptr<rhi::IRHIDevice> device;
        std::unique_ptr<rhi::IRHISwapChain> swapChain;
        CreateDeviceAndSwapChain(requestedBackend, window.hwnd, config.windowWidth, config.windowHeight, device, swapChain);

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
        InitializeImGui(window.hwnd, *device, swapChain->GetDesc().backbufferFormat, /*backbufferCount=*/2);
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

            // ImGui (optional)
            const void* imguiDrawData = BuildImGuiFrameIfEnabled(*device, rendererSettings, scene, cameraController, levelAsset, levelInstance, assets);
// Render
            renderer.SetSettings(rendererSettings);
            renderer.RenderFrame(*swapChain, scene, imguiDrawData);

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
