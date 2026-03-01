#pragma once

#include "App/Win32AppShell.h"

#if defined(CORE_USE_DX12)
#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#endif

namespace appUi
{
#if defined(CORE_USE_DX12)
    inline void InitializeImGui(HWND hwnd, rhi::IRHIDevice& device, rhi::Format backbufferFormat, int backbufferCount)
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        ImGui_ImplWin32_Init(hwnd);
        device.InitImGui(hwnd, backbufferCount, backbufferFormat);
        appWin32::g_imguiInitialized = true;

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    }

    inline void ShutdownImGui(rhi::IRHIDevice& device)
    {
        if (!appWin32::g_imguiInitialized)
        {
            return;
        }

        device.ShutdownImGui();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        appWin32::g_imguiInitialized = false;
    }

    inline const void* BuildImGuiFrameIfEnabled(
        rhi::IRHIDevice& device,
        rendern::RendererSettings& settings,
        rendern::Scene& scene,
        rendern::CameraController& cameraController,
        rendern::LevelAsset& levelAsset,
        rendern::LevelInstance& levelInstance,
        AssetManager& assets)
    {
        if (!appWin32::g_imguiInitialized || !appWin32::g_showDebugWindow || !appWin32::g_debugWindow || !appWin32::g_debugWindow->hwnd)
        {
            return nullptr;
        }

        if (!IsWindowVisible(appWin32::g_debugWindow->hwnd))
        {
            return nullptr;
        }

        device.ImGuiNewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        rendern::ui::BeginDebugDockSpace();
        rendern::ui::DrawRendererDebugUI(settings, scene, cameraController);
        rendern::ui::DrawLevelEditorUI(levelAsset, levelInstance, assets, scene, cameraController);

        ImGui::Render();
        return static_cast<const void*>(ImGui::GetDrawData());
    }

    inline rendern::InputCapture GetInputCaptureForImGui()
    {
        rendern::InputCapture capture{};
        if (appWin32::g_imguiInitialized && appWin32::g_showDebugWindow && appWin32::g_debugWindow && appWin32::g_debugWindow->hwnd)
        {
            if (IsWindowVisible(appWin32::g_debugWindow->hwnd) && GetForegroundWindow() == appWin32::g_debugWindow->hwnd)
            {
                const ImGuiIO& io = ImGui::GetIO();
                capture.captureKeyboard = io.WantCaptureKeyboard;
                capture.captureMouse = io.WantCaptureMouse;
            }
        }
        return capture;
    }

    inline void RenderImGuiToSwapChainIfEnabled(rhi::IRHIDevice& device, rhi::IRHISwapChain& swapChain, const void* imguiDrawData)
    {
        if (!imguiDrawData || !appWin32::g_imguiInitialized || !appWin32::g_showDebugWindow || !appWin32::g_debugWindow || !appWin32::g_debugWindow->hwnd)
        {
            return;
        }
        if (!IsWindowVisible(appWin32::g_debugWindow->hwnd))
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
    inline void InitializeImGui(HWND, rhi::IRHIDevice&, rhi::Format, int)
    {
    }

    inline void ShutdownImGui(rhi::IRHIDevice&)
    {
    }

    inline const void* BuildImGuiFrameIfEnabled(
        rhi::IRHIDevice&,
        rendern::RendererSettings&,
        rendern::Scene&,
        rendern::CameraController&,
        rendern::LevelAsset&,
        rendern::LevelInstance&,
        AssetManager&)
    {
        return nullptr;
    }

    inline rendern::InputCapture GetInputCaptureForImGui()
    {
        return {};
    }

    inline void RenderImGuiToSwapChainIfEnabled(rhi::IRHIDevice&, rhi::IRHISwapChain&, const void*)
    {
    }
#endif
}