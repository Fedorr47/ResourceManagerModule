#pragma once

#include "Win32AppShell.h"
#include "DebugUiHost.h"
#include "EditorViewportInteraction.h"
#include "AppRuntimeHelpers.h"
#include "AppBootstrap.h"

namespace appLifecycle
{
    struct LoadingOverlayState
    {
        bool visible = true;
        float displayProgressBar = 0.0f;
        float completedHoldSeconds = 0.0f;
    };

    struct AppConfig
    {
        int windowWidth = 1280;
        int windowHeight = 1024;
        std::wstring windowTitle = L"CoreEngineModule (DX12)";
        appRuntime::UploadBudget uploadBudget{};
    };

    struct AppState
    {
        AppConfig config{};
        rhi::Backend requestedBackend = rhi::Backend::DirectX12;
        bool canUseDebugWindow = false;

        appWin32::Win32Window window{};
#if defined(CORE_USE_DX12)
        appWin32::Win32Window debugWindow{};
        std::unique_ptr<rhi::IRHISwapChain> debugSwapChain;
#endif

        rendern::Win32Input win32Input{};
        std::unique_ptr<rhi::IRHIDevice> device;
        std::unique_ptr<rhi::IRHISwapChain> swapChain;

        StbTextureDecoder textureDecoder{};
        std::unique_ptr<rendern::JobSystemThreadPool> jobSystem;
        rendern::RenderQueueImmediate renderQueue{};
        std::unique_ptr<ITextureUploader> textureUploader;
        std::unique_ptr<TextureIO> textureIO;
        std::unique_ptr<rendern::MeshIO> meshIO;
        std::unique_ptr<AssetManager> assets;

        std::unique_ptr<rendern::LevelAsset> levelAsset;
        rendern::RendererSettings rendererSettings{};
        std::unique_ptr<rendern::Renderer> renderer;
        rendern::Scene scene{};
        std::unique_ptr<rendern::BindlessTable> bindless;
        std::unique_ptr<rendern::LevelInstance> levelInstance;
        std::unique_ptr<rendern::CameraController> cameraController;
        appEditor::EditorViewportInteraction editorViewportInteraction{};
        GameTimer frameTimer{};
        LoadingOverlayState loadingOverlay{};

        bool initialized = false;
    };

    void InitializeApp(AppState& app, int argc, char** argv);
    bool TickApp(AppState& app);
    void ShutdownApp(AppState& app);
}