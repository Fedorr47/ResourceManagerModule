module;

#include <cstdio>
#include <cstring>
#include <cctype>
#include <limits>
#include <algorithm>
#include <cmath>
#include <utility>

#if defined(CORE_USE_DX12)
#include <imgui.h>
#include <imgui_internal.h>
#endif

// Debug UI panels implemented with Dear ImGui.
// NOTE: This module is built only for DX12 backend.

export module core:imgui_debug_ui;

import :scene;
import :renderer_settings;
import :camera_controller;
import :math_utils;
import :level;
import :asset_manager;
import :assimp_scene_loader;
import :animator;
import :animation_clip;
import :animation_controller;

export namespace rendern::ui
{
    // UE-style dock host (tabs/splits). Must be called once per frame
    // before drawing individual panels/windows.
    void BeginDebugDockSpace();

    void DrawRendererDebugUI(rendern::RendererSettings& rs, rendern::Scene& scene, rendern::CameraController& camCtl);

    // Minimal Level Editor:
    // - add/remove objects (recursive delete)
    // - choose mesh/material
    // - edit transform (position/rotation/scale)
    void DrawLevelEditorUI(rendern::LevelAsset& level, rendern::LevelInstance& levelInst, AssetManager& assets, rendern::Scene& scene, rendern::CameraController& camCtl);
}

// Implementation is split into .inl files for readability.
// Keep the .inl files next to this .cppm.

#if defined(CORE_USE_DX12)

#include "ImGuiDebugUI_Common.inl"
#include "ImGuiDebugUI_RendererCore.inl"
#include "ImGuiDebugUI_Reflections.inl"
#include "ImGuiDebugUI_Light.inl"
#include "ImGuiDebugUI_RendererFacade.inl"
#include "ImGuiDebugUI_Level.inl"

namespace rendern::ui
{
    void BeginDebugDockSpace()
    {
        ImGuiIO& io = ImGui::GetIO();
        if ((io.ConfigFlags & ImGuiConfigFlags_DockingEnable) == 0)
        {
            // Docking disabled (e.g. non-docking ImGui build). Nothing to do.
            return;
        }

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(viewport->Size);
        ImGui::SetNextWindowViewport(viewport->ID);

        const ImGuiWindowFlags hostFlags =
            ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoNavFocus;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

        ImGui::Begin("##DebugDockHost", nullptr, hostFlags);
        ImGui::PopStyleVar(3);

        const ImGuiID dockId = ImGui::GetID("DebugDockSpace");
        const ImGuiDockNodeFlags dockFlags = ImGuiDockNodeFlags_PassthruCentralNode;

        ImGui::DockSpace(dockId, ImVec2(0.0f, 0.0f), dockFlags);

        // Build a default UE-like layout once.
        // User can rearrange it; ImGui will persist layout in imgui.ini.
        static bool built = false;
        if (!built)
        {
            built = true;

            ImGui::DockBuilderRemoveNode(dockId);
            ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace | dockFlags);
            ImGui::DockBuilderSetNodeSize(dockId, viewport->Size);

            // Split: left (Level Editor), right (renderer tools stack).
            ImGuiID dockLeft = 0;
            ImGuiID dockRight = 0;
            ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Left, 0.52f, &dockLeft, &dockRight);

            ImGuiID dockRightTop = 0;
            ImGuiID dockRightBottom = 0;
            ImGui::DockBuilderSplitNode(dockRight, ImGuiDir_Up, 0.58f, &dockRightTop, &dockRightBottom);

            ImGuiID dockRightBottomLeft = 0;
            ImGuiID dockRightBottomRight = 0;
            ImGui::DockBuilderSplitNode(dockRightBottom, ImGuiDir_Left, 0.52f, &dockRightBottomLeft, &dockRightBottomRight);

            ImGui::DockBuilderDockWindow("Level Editor", dockLeft);
            ImGui::DockBuilderDockWindow("Renderer / Shadows", dockRightTop);
            ImGui::DockBuilderDockWindow("Reflections", dockRightBottomLeft);
            ImGui::DockBuilderDockWindow("Lights", dockRightBottomRight);

            ImGui::DockBuilderFinish(dockId);
        }

        ImGui::End();
    }
}

#else

namespace rendern::ui
{
    void BeginDebugDockSpace()
    {
    }

    void DrawRendererDebugUI(
        rendern::RendererSettings& rs [[maybe_unused]],
        rendern::Scene& scene [[maybe_unused]],
        rendern::CameraController& camCtl [[maybe_unused]])
    {
    }

    void DrawLevelEditorUI(
        rendern::LevelAsset& level [[maybe_unused]],
        rendern::LevelInstance& levelInst [[maybe_unused]],
        AssetManager& assets [[maybe_unused]],
        rendern::Scene& scene [[maybe_unused]],
        rendern::CameraController& camCtl [[maybe_unused]])
    {
    }
}

#endif
