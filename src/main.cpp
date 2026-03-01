import core;
import std;

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "App/Win32AppShell.h"
#include "App/DebugUiHost.h"
#include "App/EditorViewportInteraction.h"
#include "App/AppRuntimeHelpers.h"

using appWin32::Win32Window;

namespace
{

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
        appRuntime::UploadBudget uploadBudget{};
    };

    // NOTE: The demo now loads its content from assets/levels/demo.level.json
    // via core:level (LevelAsset + LevelInstance).

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
        appWin32::g_showDebugWindow = canUseDebugWindow;
#endif

        appWin32::g_mainMenu = appWin32::CreateMainMenu(canUseDebugWindow, canUseDebugWindow);
        Win32Window window = appWin32::CreateWindowWin32(config.windowWidth, config.windowHeight, config.windowTitle, /*show=*/true, appWin32::g_mainMenu);
        appWin32::g_window = &window;

#if defined(CORE_USE_DX12)
        Win32Window debugWindow{};
        std::unique_ptr<rhi::IRHISwapChain> debugSwapChain;
        if (requestedBackend == rhi::Backend::DirectX12)
        {
            debugWindow = appWin32::CreateWindowWin32(900, 900, L"CoreEngineModule - Debug UI", /*show=*/appWin32::g_showDebugWindow);
            appWin32::g_debugWindow = &debugWindow;
            appWin32::UpdateMainMenuDebugWindowCheck();
        }
#endif

        rendern::Win32Input win32Input{};
        appWin32::g_input = &win32Input;

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

        // Level asset (JSON)
        rendern::LevelAsset levelAsset = rendern::LoadLevelAssetFromJson("levels/demo.level.json");

        // Renderer (facade) - Stage1 expects Scene
        rendern::RendererSettings rendererSettings{};
        rendererSettings.drawLightGizmos = true;
        rendern::Renderer renderer{ *device, rendererSettings };

#if defined(CORE_USE_DX12)
        if (requestedBackend == rhi::Backend::DirectX12 && debugSwapChain && debugWindow.hwnd)
        {
            appUi::InitializeImGui(debugWindow.hwnd, *device, debugSwapChain->GetDesc().backbufferFormat, /*backbufferCount=*/2);
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

        appEditor::EditorViewportInteraction editorViewportInteraction{};

        // Timer
        GameTimer frameTimer{};
        frameTimer.SetMaxDelta(0.05);
        frameTimer.Reset();

        while (window.running)
        {
            appWin32::PumpMessages(window);
            if (!window.running)
            {
                break;
            }

            appRuntime::ApplyPendingResize(window, swapChain.get());

#if defined(CORE_USE_DX12)
            if (debugWindow.hwnd)
            {
                appRuntime::ApplyPendingResize(debugWindow, debugSwapChain.get());
            }
#endif

            if (appRuntime::ShouldSkipMainViewportFrame(window))
            {
                appWin32::TinySleep();
                continue;
            }

            appRuntime::DriveAssetStreaming(assets, levelInstance, bindless, scene, config.uploadBudget);

            // Delta time
            frameTimer.Tick();
            const float deltaSeconds = static_cast<float>(frameTimer.GetDeltaTime());

            // Input + camera controller
            win32Input.SetCaptureMode(appUi::GetInputCaptureForImGui());
            win32Input.NewFrame(window.hwnd);
            const rendern::InputState& inputState = win32Input.State();
            cameraController.Update(deltaSeconds, inputState, scene.camera);

            appEditor::ApplyGizmoModeHotkeys(editorViewportInteraction, scene, inputState);
            appEditor::SyncEditorGizmoVisuals(editorViewportInteraction, levelAsset, levelInstance, scene);
            appEditor::UpdateViewportGizmoHover(editorViewportInteraction, window.hwnd, window.width, window.height, scene, inputState);
            appEditor::HandleViewportMouseInteraction(
                editorViewportInteraction,
                window.hwnd,
                window.width,
                window.height,
                levelAsset,
                levelInstance,
                scene,
                inputState);

            // ImGui (optional) - rendered into a separate debug window swapchain
            const void* imguiDrawData = appUi::BuildImGuiFrameIfEnabled(*device, rendererSettings, scene, cameraController, levelAsset, levelInstance, assets);

            // Render main scene (no UI overlay)
            renderer.SetSettings(rendererSettings);
            renderer.RenderFrame(*swapChain, scene, /*imguiDrawData=*/nullptr);

#if defined(CORE_USE_DX12)
            if (appRuntime::CanRenderDebugSwapChain(debugWindow, debugSwapChain.get()))
            {
                appUi::RenderImGuiToSwapChainIfEnabled(*device, *debugSwapChain, imguiDrawData);
            }
#endif

            appWin32::TinySleep();
        }

        appRuntime::ShutdownRuntime(
            *device,
            renderer,
            levelInstance,
            bindless,
            jobSystem,
            assets,
            window
#if defined(CORE_USE_DX12)
            , &debugWindow
#endif
        );

        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Fatal: " << exception.what() << "\n";
        return 2;
    }
}