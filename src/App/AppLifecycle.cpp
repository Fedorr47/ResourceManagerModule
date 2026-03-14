import core;
import std;

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "AppLifecycle.h"

namespace appLifecycle
{
    static std::uint32_t ComputeStreamingWorkerCount() noexcept
    {
        const unsigned int hc = std::thread::hardware_concurrency();
        if (hc <= 1u)
        {
            return 1u;
        }

        unsigned int wc = hc - 1u;
        if (wc < 1u)
            wc = 1u;
        if (wc > 8u)
            wc = 8u;

        return static_cast<std::uint32_t>(wc);
    }

    static void ResetEditorInteractionState(AppState& app)
    {
        appEditor::EndAllGizmoDrags(app.editorViewportInteraction, app.scene);
        appEditor::ClearAllGizmoHover(app.editorViewportInteraction, app.scene);
        appEditor::ResetGizmoState(app.scene.editorTranslateGizmo);
        appEditor::ResetGizmoState(app.scene.editorRotateGizmo);
        appEditor::ResetGizmoState(app.scene.editorScaleGizmo);
        app.scene.editorParticleEmitterTranslateDrag = {};
        app.scene.EditorClearSelection();
    }


    void InitializeApp(AppState& app, int argc, char** argv)
    {
        app.requestedBackend = appBootstrap::ParseBackendFromArgs(argc, argv);
        app.canUseDebugWindow = appBootstrap::CanUseDebugWindow(app.requestedBackend);

        appBootstrap::CreatePrimaryWindowSet(
            app.config.windowWidth,
            app.config.windowHeight,
            app.config.windowTitle,
            app.canUseDebugWindow,
            app.window
#if defined(CORE_USE_DX12)
            , &app.debugWindow
#endif
        );

        appBootstrap::BindWin32Input(app.win32Input);
        appBootstrap::CreateDeviceAndSwapChain(
            app.requestedBackend,
            app.window.hwnd,
            app.config.windowWidth,
            app.config.windowHeight,
            app.device,
            app.swapChain);

#if defined(CORE_USE_DX12)
        appBootstrap::CreateDebugSwapChainIfNeeded(app.requestedBackend, *app.device, app.debugWindow, app.debugSwapChain);
#endif

        app.jobSystem = std::make_unique<rendern::JobSystemThreadPool>(ComputeStreamingWorkerCount());

        app.textureUploader = appBootstrap::CreateTextureUploader(app.device->GetBackend(), *app.device);
        app.textureIO = std::make_unique<TextureIO>(app.textureDecoder, *app.textureUploader, *app.jobSystem, app.renderQueue);
        app.meshIO = std::make_unique<rendern::MeshIO>(*app.device, *app.jobSystem, app.renderQueue);
        app.assets = std::make_unique<AssetManager>(*app.textureIO, *app.meshIO);

        app.levelAsset = std::make_unique<rendern::LevelAsset>(rendern::LoadLevelAssetFromJson("levels/demo.level.with_fsm_test.locomotion.phaseB.json"));

        app.rendererSettings.drawLightGizmos = true;
        app.rendererSettings.loadingOverlayVisible = true;
        app.rendererSettings.loadingOverlayProgressBar = 0.0f;
        app.renderer = std::make_unique<rendern::Renderer>(*app.device, app.rendererSettings);

#if defined(CORE_USE_DX12)
        if (app.requestedBackend == rhi::Backend::DirectX12 && app.debugSwapChain && app.debugWindow.hwnd)
        {
            appUi::InitializeImGui(app.debugWindow.hwnd, *app.device, app.debugSwapChain->GetDesc().backbufferFormat, /*backbufferCount=*/2);
        }
#endif

        app.scene.Clear();
        app.bindless = std::make_unique<rendern::BindlessTable>(*app.device);
        app.levelInstance = std::make_unique<rendern::LevelInstance>(rendern::InstantiateLevel(
            app.scene,
            *app.assets,
            *app.bindless,
            *app.levelAsset,
            mathUtils::Mat4(1.0f)));

        app.gameplayRuntime = std::make_unique<rendern::GameplayRuntime>();
        app.gameplayRuntime->Initialize(*app.levelAsset, *app.levelInstance, app.scene);

        app.cameraController = std::make_unique<rendern::CameraController>();
        app.cameraController->ResetFromCamera(app.scene.camera);

        app.gameplayMode = rendern::GameplayRuntimeMode::Editor;

        app.frameTimer.SetMaxDelta(0.05);
        app.frameTimer.Reset();
        app.initialized = true;
    }

    bool TickApp(AppState& app)
    {
        appWin32::PumpMessages(app.window);
        if (!app.window.running)
        {
            return false;
        }

        appRuntime::ApplyPendingResize(app.window, app.swapChain.get());
#if defined(CORE_USE_DX12)
        appRuntime::ApplyPendingResize(app.debugWindow, app.debugSwapChain.get());
#endif

        if (appRuntime::ShouldSkipMainViewportFrame(app.window))
        {
            appWin32::TinySleep();
            return true;
        }

        appRuntime::DriveAssetStreaming(*app.assets, *app.levelInstance, *app.bindless, app.scene, app.config.uploadBudget);

        app.frameTimer.Tick();
        const float deltaSeconds = static_cast<float>(app.frameTimer.GetDeltaTime());

        const AssetStreamingStats streamingStats = app.assets->GetStreamingStats();
        const bool hasPendingStreaming = streamingStats.HasPendingWork();
        const float targetProgress01 = streamingStats.Completion01();

        auto& overlay = app.loadingOverlay;
        const float lerpAlpha = std::clamp(deltaSeconds * (hasPendingStreaming ? 4.0f : 10.0f), 0.0f, 1.0f);
        overlay.displayProgressBar = std::lerp(overlay.displayProgressBar, targetProgress01, lerpAlpha);

        if (hasPendingStreaming)
        {
            overlay.visible = true;
            overlay.completedHoldSeconds = 0.0f;
        }
        else
        {
            overlay.displayProgressBar = std::max(overlay.displayProgressBar, 1.0f);
            overlay.completedHoldSeconds += deltaSeconds;
            if (overlay.completedHoldSeconds >= 0.35f)
            {
                overlay.visible = false;
            }
        }

        app.rendererSettings.loadingOverlayVisible = overlay.visible;
        app.rendererSettings.loadingOverlayProgressBar = overlay.visible
            ? std::clamp(overlay.displayProgressBar, hasPendingStreaming ? 0.02f : 1.0f, 1.0f)
            : 0.0f;

        app.rendererSettings.loadingOverlayTotalUnits = streamingStats.total.totalEntries;
        app.rendererSettings.loadingOverlayCompletedUnits = streamingStats.total.loadedEntries + streamingStats.total.failedEntries;

        app.win32Input.SetCaptureMode(appUi::GetInputCaptureForImGui());
        app.win32Input.NewFrame(app.window.hwnd);

        if (app.win32Input.State().KeyPressed(VK_F5))
        {
            app.gameplayMode = (app.gameplayMode == rendern::GameplayRuntimeMode::Editor)
                ? rendern::GameplayRuntimeMode::Game
                : rendern::GameplayRuntimeMode::Editor;
        }

        if (app.gameplayMode == rendern::GameplayRuntimeMode::Game)
        {
            ResetEditorInteractionState(app);
        }
        else
        {
            app.cameraController->Update(deltaSeconds, app.win32Input.State(), app.scene.camera);
            appEditor::ApplyGizmoModeHotkeys(app.editorViewportInteraction, app.scene, app.win32Input.State());
            appEditor::SyncEditorGizmoVisuals(app.editorViewportInteraction, *app.levelAsset, *app.levelInstance, app.scene);
            appEditor::UpdateViewportGizmoHover(app.editorViewportInteraction, app.window.hwnd, app.window.width, app.window.height, app.scene, app.win32Input.State());
            appEditor::HandleViewportMouseInteraction(app.editorViewportInteraction, app.window.hwnd, app.window.width, app.window.height, *app.levelAsset, *app.levelInstance, app.scene, app.win32Input.State());
        }

        if (app.gameplayRuntime)
        {
            rendern::GameplayUpdateContext gameplayCtx{};
            gameplayCtx.deltaSeconds = deltaSeconds;
            gameplayCtx.mode = app.gameplayMode;
            gameplayCtx.input = &app.win32Input.State();
            gameplayCtx.levelAsset = app.levelAsset.get();
            gameplayCtx.levelInstance = app.levelInstance.get();
            gameplayCtx.scene = &app.scene;

            app.gameplayRuntime->BeginFrame();
            app.gameplayRuntime->PreAnimationUpdate(gameplayCtx);
            if (app.gameplayMode == rendern::GameplayRuntimeMode::Game)
            {
                app.cameraController->ResetFromCamera(app.scene.camera);
            }
        }

        app.scene.UpdateSkinned(deltaSeconds);

        if (app.gameplayRuntime)
        {
            rendern::GameplayUpdateContext gameplayCtx{};
            gameplayCtx.deltaSeconds = deltaSeconds;
            gameplayCtx.mode = app.gameplayMode;
            gameplayCtx.input = &app.win32Input.State();
            gameplayCtx.levelAsset = app.levelAsset.get();
            gameplayCtx.levelInstance = app.levelInstance.get();
            gameplayCtx.scene = &app.scene;
            app.gameplayRuntime->PostAnimationUpdate(gameplayCtx);
        }

        app.scene.UpdateParticles(deltaSeconds);

        const void* imguiDrawData = appUi::BuildImGuiFrameIfEnabled(
            *app.device,
            app.rendererSettings,
            app.scene,
            *app.cameraController,
            *app.levelAsset,
            *app.levelInstance,
            *app.assets,
            app.gameplayMode);

        app.renderer->SetSettings(app.rendererSettings);
        app.renderer->RenderFrame(*app.swapChain, app.scene, /*imguiDrawData=*/nullptr);

#if defined(CORE_USE_DX12)
        if (appRuntime::CanRenderDebugSwapChain(app.debugWindow, app.debugSwapChain.get()))
        {
            appUi::RenderImGuiToSwapChainIfEnabled(*app.device, *app.debugSwapChain, imguiDrawData);
        }
#endif

        appWin32::TinySleep();
        return true;
    }

    void ShutdownApp(AppState& app)
    {
        if (!app.initialized)
        {
            return;
        }

        appRuntime::ShutdownRuntime(
            *app.device,
            *app.renderer,
            *app.levelInstance,
            *app.bindless,
            *app.jobSystem,
            *app.assets,
            app.window
#if defined(CORE_USE_DX12)
            , &app.debugWindow
#endif
        );

#if defined(CORE_USE_DX12)
        app.debugSwapChain.reset();
#endif
        app.swapChain.reset();
        app.renderer.reset();
        app.gameplayRuntime.reset();
        app.levelInstance.reset();
        app.bindless.reset();
        app.levelAsset.reset();
        app.assets.reset();
        app.meshIO.reset();
        app.textureIO.reset();
        app.textureUploader.reset();
        app.jobSystem.reset();
        app.device.reset();
        app.cameraController.reset();
        app.initialized = false;
    }
}
