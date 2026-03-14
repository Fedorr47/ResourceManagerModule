#pragma once

#include "Win32AppShell.h"

namespace appUi
{
    void InitializeImGui(HWND hwnd, rhi::IRHIDevice& device, rhi::Format backbufferFormat, int backbufferCount);
    void ShutdownImGui(rhi::IRHIDevice& device);

    const void* BuildImGuiFrameIfEnabled(
        rhi::IRHIDevice& device,
        rendern::RendererSettings& settings,
        rendern::Scene& scene,
        rendern::CameraController& cameraController,
        rendern::LevelAsset& levelAsset,
        rendern::LevelInstance& levelInstance,
        AssetManager& assets,
        rendern::GameplayRuntimeMode& runtimeMode);

    rendern::InputCapture GetInputCaptureForImGui();
    void RenderImGuiToSwapChainIfEnabled(rhi::IRHIDevice& device, rhi::IRHISwapChain& swapChain, const void* imguiDrawData);
}
