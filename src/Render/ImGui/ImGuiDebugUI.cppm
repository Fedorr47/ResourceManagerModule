module;

#include <cstdio>
#include <cstring>
#include <cctype>
#include <limits>
#include <algorithm>
#include <cmath>

#if defined(CORE_USE_DX12)
#include <imgui.h>
#endif

// Debug UI panels implemented with Dear ImGui.
// NOTE: This module is built only for DX12 backend.

export module core:imgui_debug_ui;

import std;

import :scene;
import :renderer_settings;
import :camera_controller;
import :math_utils;
import :level;
import :asset_manager;

export namespace rendern::ui
{
    void DrawRendererDebugUI(rendern::RendererSettings& rs, rendern::Scene& scene, rendern::CameraController& camCtl);

    // Minimal Level Editor:
    // - add/remove objects (recursive delete)
    // - choose mesh/material
    // - edit transform (position/rotation/scale)
    void DrawLevelEditorUI(rendern::LevelAsset& level, rendern::LevelInstance& levelInst, AssetManager& assets, rendern::Scene& scene, rendern::CameraController& camCtl);
}

namespace rendern::ui
{
    namespace
    {
        constexpr const char* kLightTypeNames[] = { "Directional", "Point", "Spot" };

        static int ToIndex(rendern::LightType t)
        {
            switch (t)
            {
            case rendern::LightType::Directional: return 0;
            case rendern::LightType::Point:       return 1;
            case rendern::LightType::Spot:        return 2;
            default:                             return 0;
            }
        }

        static rendern::LightType FromIndex(int i)
        {
            switch (i)
            {
            case 0: return rendern::LightType::Directional;
            case 1: return rendern::LightType::Point;
            case 2: return rendern::LightType::Spot;
            default: return rendern::LightType::Directional;
            }
        }

        static void EnsureNormalized(mathUtils::Vec3& v)
        {
            const float len2 = v.x * v.x + v.y * v.y + v.z * v.z;
            if (len2 > 1e-12f)
            {
                v = mathUtils::Normalize(v);
            }
            else
            {
                v = { 0.0f, -1.0f, 0.0f };
            }
        }

        static bool DragVec3(const char* label, mathUtils::Vec3& v, float speed = 0.05f, float minv = 0.0f, float maxv = 0.0f)
        {
            float a[3] = { v.x, v.y, v.z };
            const bool changed = ImGui::DragFloat3(label, a, speed, minv, maxv, "%.3f");
            if (changed)
            {
                v.x = a[0];
                v.y = a[1];
                v.z = a[2];
            }
            return changed;
        }

        static bool Color3(const char* label, mathUtils::Vec3& v)
        {
            float a[3] = { v.x, v.y, v.z };
            const bool changed = ImGui::ColorEdit3(label, a);
            if (changed)
            {
                v.x = a[0];
                v.y = a[1];
                v.z = a[2];
            }
            return changed;
        }

        static mathUtils::Mat4 GetViewProj(const ImVec2& displaySize, Camera camera)
        {
            const float aspect = (displaySize.y > 0.0f) ? (displaySize.x / displaySize.y) : 1.0f;

            const mathUtils::Mat4 projection =
                mathUtils::PerspectiveRH_ZO(mathUtils::DegToRad(camera.fovYDeg), aspect, camera.nearZ, camera.farZ);

            const mathUtils::Mat4 view =
                mathUtils::LookAt(camera.position, camera.target, camera.up);

            return projection * view;
        }

        static void ApplyDefaultsForType(rendern::Light& l)
        {
            if (l.type == rendern::LightType::Directional)
            {
                l.position = { 0.0f, 0.0f, 0.0f };
                l.direction = { -0.4f, -1.0f, -0.3f };
                EnsureNormalized(l.direction);
                l.color = { 1.0f, 1.0f, 1.0f };
                l.intensity = 0.5f;
            }
            else if (l.type == rendern::LightType::Point)
            {
                l.position = { 0.0f, 5.0f, 0.0f };
                l.direction = { 0.0f, -1.0f, 0.0f };
                l.color = { 1.0f, 1.0f, 1.0f };
                l.intensity = 1.0f;
                l.range = 30.0f;
                l.attConstant = 1.0f;
                l.attLinear = 0.09f;
                l.attQuadratic = 0.032f;
            }
            else // Spot
            {
                l.position = { 2.0f, 4.0f, 2.0f };
                l.direction = { -1.0f, -2.0f, -1.0f };
                EnsureNormalized(l.direction);
                l.color = { 1.0f, 1.0f, 1.0f };
                l.intensity = 5.0f;
                l.range = 50.0f;
                l.innerHalfAngleDeg = 20.0f;
                l.outerHalfAngleDeg = 35.0f;
                l.attConstant = 1.0f;
                l.attLinear = 0.09f;
                l.attQuadratic = 0.032f;
            }
        }

        static void DrawOneLightEditor(rendern::Light& l, std::size_t idx)
        {
            ImGui::PushID(static_cast<int>(idx));

            int typeIdx = ToIndex(l.type);
            if (ImGui::Combo("Type", &typeIdx, kLightTypeNames, 3))
            {
                l.type = FromIndex(typeIdx);
                ApplyDefaultsForType(l);
            }

            Color3("Color", l.color);
            ImGui::DragFloat("Intensity", &l.intensity, 0.01f, 0.0f, 200.0f, "%.3f");

            ImGui::Separator();

            switch (l.type)
            {
            case rendern::LightType::Directional:
                DragVec3("Direction", l.direction, 0.02f, -1.0f, 1.0f);
                if (ImGui::Button("Normalize direction"))
                    EnsureNormalized(l.direction);
                break;

            case rendern::LightType::Point:
                DragVec3("Position", l.position, 0.05f);
                ImGui::DragFloat("Range", &l.range, 0.1f, 0.1f, 500.0f, "%.2f");
                ImGui::DragFloat("Att const", &l.attConstant, 0.01f, 0.0f, 10.0f, "%.3f");
                ImGui::DragFloat("Att linear", &l.attLinear, 0.001f, 0.0f, 10.0f, "%.4f");
                ImGui::DragFloat("Att quad", &l.attQuadratic, 0.001f, 0.0f, 10.0f, "%.5f");
                break;

            case rendern::LightType::Spot:
                DragVec3("Position", l.position, 0.05f);
                DragVec3("Direction", l.direction, 0.02f, -1.0f, 1.0f);
                if (ImGui::Button("Normalize direction"))
                    EnsureNormalized(l.direction);
                ImGui::DragFloat("Range", &l.range, 0.1f, 0.1f, 500.0f, "%.2f");
                ImGui::DragFloat("Inner half angle", &l.innerHalfAngleDeg, 0.1f, 0.0f, 89.0f, "%.2f deg");
                ImGui::DragFloat("Outer half angle", &l.outerHalfAngleDeg, 0.1f, 0.0f, 89.0f, "%.2f deg");
                if (l.innerHalfAngleDeg > l.outerHalfAngleDeg)
                    l.innerHalfAngleDeg = l.outerHalfAngleDeg;
                ImGui::DragFloat("Att const", &l.attConstant, 0.01f, 0.0f, 10.0f, "%.3f");
                ImGui::DragFloat("Att linear", &l.attLinear, 0.001f, 0.0f, 10.0f, "%.4f");
                ImGui::DragFloat("Att quad", &l.attQuadratic, 0.001f, 0.0f, 10.0f, "%.5f");
                break;
            }

            ImGui::PopID();
        }
        struct Ray
        {
            mathUtils::Vec3 origin{ 0.0f, 0.0f, 0.0f };
            mathUtils::Vec3 dir{ 0.0f, 0.0f, 1.0f }; // normalized
        };

        static mathUtils::Vec3 MinVec3(const mathUtils::Vec3& a, const mathUtils::Vec3& b) noexcept
        {
            return { std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z) };
        }

        static mathUtils::Vec3 MaxVec3(const mathUtils::Vec3& a, const mathUtils::Vec3& b) noexcept
        {
            return { std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z) };
        }

        static mathUtils::Vec3 TransformPoint(const mathUtils::Mat4& m, const mathUtils::Vec3& p) noexcept
        {
            const mathUtils::Vec4 w = m * mathUtils::Vec4(p, 1.0f);
            return { w.x, w.y, w.z };
        }

        static void TransformAABB(const mathUtils::Vec3& bmin, const mathUtils::Vec3& bmax, const mathUtils::Mat4& m,
            mathUtils::Vec3& outMin, mathUtils::Vec3& outMax) noexcept
        {
            const mathUtils::Vec3 c[8] =
            {
                { bmin.x, bmin.y, bmin.z }, { bmax.x, bmin.y, bmin.z }, { bmin.x, bmax.y, bmin.z }, { bmax.x, bmax.y, bmin.z },
                { bmin.x, bmin.y, bmax.z }, { bmax.x, bmin.y, bmax.z }, { bmin.x, bmax.y, bmax.z }, { bmax.x, bmax.y, bmax.z },
            };

            mathUtils::Vec3 wmin{ std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity() };
            mathUtils::Vec3 wmax{ -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity() };

            for (const auto& p : c)
            {
                const mathUtils::Vec3 wp = TransformPoint(m, p);
                wmin = MinVec3(wmin, wp);
                wmax = MaxVec3(wmax, wp);
            }

            outMin = wmin;
            outMax = wmax;
        }
        static bool IntersectRayAABB(const Ray& ray, const mathUtils::Vec3& bmin, const mathUtils::Vec3& bmax, float& outT) noexcept
        {
            float tmin = 0.0f;
            float tmax = std::numeric_limits<float>::infinity();

            const float o[3] = { ray.origin.x, ray.origin.y, ray.origin.z };
            const float d[3] = { ray.dir.x, ray.dir.y, ray.dir.z };
            const float mn[3] = { bmin.x, bmin.y, bmin.z };
            const float mx[3] = { bmax.x, bmax.y, bmax.z };

            for (int axis = 0; axis < 3; ++axis)
            {
                const float dir = d[axis];
                const float ori = o[axis];

                if (std::abs(dir) < 1e-8f)
                {
                    if (ori < mn[axis] || ori > mx[axis])
                    {
                        return false;
                    }
                    continue;
                }

                const float invD = 1.0f / dir;
                float t1 = (mn[axis] - ori) * invD;
                float t2 = (mx[axis] - ori) * invD;
                if (t1 > t2)
                {
                    std::swap(t1, t2);
                }

                tmin = std::max(tmin, t1);
                tmax = std::min(tmax, t2);
                if (tmin > tmax)
                {
                    return false;
                }
            }

            outT = tmin;
            return true;
        }

        static Ray BuildMouseRay(
            const rendern::Scene& scene, 
            const rendern::CameraController& camCtl,
            const ImVec2& mousePos, 
            const ImVec2& displaySize) noexcept
        {
            const float width = (displaySize.x > 1.0f) ? displaySize.x : 1.0f;
            const float height = (displaySize.y > 1.0f) ? displaySize.y : 1.0f;

            // NDC in [-1..1], with +Y up.
            const float ndcX = (mousePos.x / width) * 2.0f - 1.0f;
            const float ndcY = 1.0f - (mousePos.y / height) * 2.0f;

            const float aspect = width / height;
            const float tanHalfFov = std::tan(mathUtils::DegToRad(scene.camera.fovYDeg) * 0.5f);

            const mathUtils::Vec3 forward = mathUtils::Normalize(scene.camera.target - scene.camera.position);
            const mathUtils::Vec3 right = mathUtils::Normalize(mathUtils::Cross(forward, scene.camera.up));
            const mathUtils::Vec3 up = mathUtils::Normalize(mathUtils::Cross(right, forward));

            mathUtils::Vec3 dir = forward;
            dir = dir + right * (ndcX * aspect * tanHalfFov);
            dir = dir + up * (ndcY * tanHalfFov);
            dir = mathUtils::Normalize(dir);

            Ray ray;
            ray.origin = scene.camera.position;
            ray.dir = dir;
            return ray;
        }

        static int PickNodeUnderMouse(
            const rendern::Scene& scene, 
            const rendern::LevelInstance& levelInst,
            const ImVec2& mousePos, 
            const ImVec2& displaySize, 
            const rendern::CameraController& camCtl,
            float& outBestT,
            Ray& outRay)
        {
            outRay = BuildMouseRay(scene, camCtl, mousePos, displaySize);

            float bestT = std::numeric_limits<float>::infinity();
            int bestNode = -1;

            for (int di = 0; di < static_cast<int>(scene.drawItems.size()); ++di)
            {
                const int nodeIndex = levelInst.GetNodeIndexFromDrawIndex(di);
                if (nodeIndex < 0)
                {
                    continue;
                }

                const rendern::DrawItem& item = scene.drawItems[static_cast<std::size_t>(di)];
                if (!item.mesh)
                {
                    continue;
                }

                const auto& meshBounds = item.mesh->GetBounds();
                mathUtils::Vec3 wmin{}, wmax{};
                TransformAABB(meshBounds.aabbMin, meshBounds.aabbMax, item.transform.ToMatrix(), wmin, wmax);

                float t = 0.0f;
                if (IntersectRayAABB(outRay, wmin, wmax, t))
                {
                    if (t < bestT)
                    {
                        bestT = t;
                        bestNode = nodeIndex;
                    }
                }
            }

            outBestT = bestT;
            return bestNode;
        }
        // ------------------------------------------------------------
        // Light header row with actions on the right (clickable)
        // ------------------------------------------------------------
        static bool LightHeaderWithActions(const char* headerText,
            bool defaultOpen,
            bool& enabled,
            bool& doDelete)
        {
            doDelete = false;

            ImGuiTreeNodeFlags flags =
                ImGuiTreeNodeFlags_Framed |
                ImGuiTreeNodeFlags_SpanAvailWidth |
                ImGuiTreeNodeFlags_AllowOverlap |
                ImGuiTreeNodeFlags_FramePadding;

            if (defaultOpen)
                flags |= ImGuiTreeNodeFlags_DefaultOpen;

            // Tree node label is hidden; we render text via format string to keep ID stable.
            const bool open = ImGui::TreeNodeEx("##light_node", flags, "%s", headerText);

            // The last item is the header frame; place our controls on top-right of it.
            const ImVec2 rmin = ImGui::GetItemRectMin();
            const ImVec2 rmax = ImGui::GetItemRectMax();

            // Reserve width for controls (tweakable).
            const float deleteW = 62.0f; // typical "Delete" button width
            const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
            const float checkW = ImGui::GetFrameHeight(); // checkbox square
            const float totalW = checkW + spacing + deleteW;

            // Align to the center vertically.
            const float y = rmin.y + (rmax.y - rmin.y - ImGui::GetFrameHeight()) * 0.5f;

            // Start X a bit before the right edge.
            const float x = rmax.x - totalW - spacing;

            // Draw controls on the same line, but overlayed.
            ImGui::SetCursorScreenPos(ImVec2(x, y));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 2.0f));

            // Checkbox (no label text to stay compact)
            ImGui::Checkbox("##Enabled", &enabled);
            ImGui::SameLine();
            doDelete = ImGui::Button("Delete");

            ImGui::PopStyleVar();

            return open;
        }
    }

    void DrawRendererDebugUI(rendern::RendererSettings& rs, rendern::Scene& scene, rendern::CameraController& camCtl)
    {
        ImGui::Begin("Renderer / Shadows");

        ImGui::Checkbox("Depth prepass", &rs.enableDepthPrepass);
        ImGui::Checkbox("Frustum culling", &rs.enableFrustumCulling);
        ImGui::Checkbox("Debug print draw calls", &rs.debugPrintDrawCalls);

        // ------------------------------------------------------------
        // Camera
        // ------------------------------------------------------------
        if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen))
        {
            rendern::Camera& cam = scene.camera;

            if (DragVec3("Position", cam.position, 0.05f))
            {
                cam.target = cam.position + camCtl.Forward();
            }
            if (DragVec3("Target", cam.target, 0.05f))
            {
                camCtl.ResetFromCamera(cam);
            }

            constexpr float kRadToDeg = 57.29577951308232f;
            constexpr float kDegToRad = 0.017453292519943295f;

            float yawDeg = camCtl.YawRad() * kRadToDeg;
            float pitchDeg = camCtl.PitchRad() * kRadToDeg;

            bool changedAngles = false;
            changedAngles |= ImGui::SliderFloat("Yaw (deg)", &yawDeg, -180.0f, 180.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
            changedAngles |= ImGui::SliderFloat("Pitch (deg)", &pitchDeg, -89.0f, 89.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);

            if (changedAngles)
            {
                camCtl.SetYawPitchRad(yawDeg * kDegToRad, pitchDeg * kDegToRad, cam);
            }

            ImGui::SliderFloat("FOV Y (deg)", &cam.fovYDeg, 20.0f, 120.0f);
            ImGui::InputFloat("Near Z", &cam.nearZ, 0.01f, 0.1f, "%.4f");
            ImGui::InputFloat("Far Z", &cam.farZ, 1.0f, 10.0f, "%.1f");

            auto& s = camCtl.Settings();

            bool enabledCtl = camCtl.Enabled();
            if (ImGui::Checkbox("Enable controller", &enabledCtl))
            {
                camCtl.SetEnabled(enabledCtl);
            }
            ImGui::Checkbox("Invert Y", &s.invertY);
            ImGui::SliderFloat("Move speed", &s.moveSpeed, 0.1f, 50.0f);
            ImGui::SliderFloat("Sprint multiplier", &s.sprintMultiplier, 1.0f, 12.0f);
            ImGui::SliderFloat("Mouse sensitivity", &s.mouseSensitivity, 0.0005f, 0.01f, "%.4f", ImGuiSliderFlags_Logarithmic);

            if (ImGui::Button("Reset view"))
            {
                cam.position = mathUtils::Vec3(5.0f, 10.0f, 10.0f);
                cam.target = mathUtils::Vec3(0.0f, 0.0f, 0.0f);
                cam.up = mathUtils::Vec3(0.0f, 1.0f, 0.0f);
                cam.fovYDeg = 60.0f;
                cam.nearZ = 0.01f;
                cam.farZ = 200.0f;
                camCtl.ResetFromCamera(cam);
            }

            ImGui::TextDisabled("Controls: hold RMB to look, WASD move, QE up/down, Shift sprint");
            ImGui::Separator();
        }

        ImGui::Separator();
        ImGui::Text("Shadow bias (texels)");
        ImGui::SliderFloat("Dir base", &rs.dirShadowBaseBiasTexels, 0.0f, 5.0f, "%.3f");
        ImGui::SliderFloat("Spot base", &rs.spotShadowBaseBiasTexels, 0.0f, 10.0f, "%.3f");
        ImGui::SliderFloat("Point base", &rs.pointShadowBaseBiasTexels, 0.0f, 10.0f, "%.3f");
        ImGui::SliderFloat("Slope scale", &rs.shadowSlopeScaleTexels, 0.0f, 10.0f, "%.3f");

        ImGui::Separator();
        ImGui::Text("Debug draw");
        ImGui::Checkbox("Light gizmos (3D)", &rs.drawLightGizmos);
        ImGui::Checkbox("Depth test (gizmos)", &rs.debugDrawDepthTest);
        if (rs.drawLightGizmos)
        {
            ImGui::SliderFloat("Gizmo half-size", &rs.lightGizmoHalfSize, 0.01f, 2.0f, "%.3f");
            ImGui::SliderFloat("Arrow length", &rs.lightGizmoArrowLength, 0.05f, 25.0f, "%.3f");
            ImGui::SliderFloat("Arrow thickness", &rs.lightGizmoArrowThickness, 0.001f, 2.0f, "%.3f");
            }

        ImGui::Separator();
        if (ImGui::CollapsingHeader("Lights", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Count: %d", static_cast<int>(scene.lights.size()));

            if (ImGui::Button("Add Directional"))
            {
                rendern::Light l{};
                l.type = rendern::LightType::Directional;
                ApplyDefaultsForType(l);
                scene.AddLight(l);
            }
            ImGui::SameLine();
            if (ImGui::Button("Add Point"))
            {
                rendern::Light l{};
                l.type = rendern::LightType::Point;
                ApplyDefaultsForType(l);
                scene.AddLight(l);
            }
            ImGui::SameLine();
            if (ImGui::Button("Add Spot"))
            {
                rendern::Light l{};
                l.type = rendern::LightType::Spot;
                ApplyDefaultsForType(l);
                scene.AddLight(l);
            }

            ImGui::Spacing();

            // Track previous intensities for a simple Enabled toggle.
            static std::vector<float> prevIntensity;
            if (prevIntensity.size() != scene.lights.size())
                prevIntensity.resize(scene.lights.size(), 1.0f);

            for (std::size_t i = 0; i < scene.lights.size();)
            {
                auto& l = scene.lights[i];

                ImGui::PushID(static_cast<int>(i));

                const char* typeName = kLightTypeNames[ToIndex(l.type)];
                char header[64]{};
                std::snprintf(header, sizeof(header), "[%s] #%zu", typeName, i);

                // Enabled state derived from intensity.
                bool enabled = (l.intensity > 0.00001f);
                bool doDelete = false;

                const bool open = LightHeaderWithActions(header, true, enabled, doDelete);

                // Apply "Enabled" change
                if (!enabled && l.intensity > 0.0f)
                {
                    prevIntensity[i] = std::max(prevIntensity[i], l.intensity);
                    l.intensity = 0.0f;
                }
                else if (enabled && l.intensity <= 0.00001f)
                {
                    l.intensity = (prevIntensity[i] > 0.0f) ? prevIntensity[i] : 1.0f;
                }

                if (open)
                {
                    DrawOneLightEditor(l, i);
                    ImGui::TreePop();
                }

                ImGui::PopID();

                if (doDelete)
                {
                    scene.lights.erase(scene.lights.begin() + static_cast<std::ptrdiff_t>(i));
                    prevIntensity.erase(prevIntensity.begin() + static_cast<std::ptrdiff_t>(i));
                    continue;
                }

                ++i;
            }
        }

        ImGui::Separator();
        ImGui::Text("F1: toggle UI");
        ImGui::End();
    }

    void DrawLevelEditorUI(
        rendern::LevelAsset& level,
        rendern::LevelInstance& levelInst, 
        AssetManager& assets, 
        rendern::Scene& scene,
        rendern::CameraController& camCtl)
    {
        ImGui::Begin("Level Editor");

        ImGui::Text("Nodes: %d   DrawItems: %d", static_cast<int>(level.nodes.size()), static_cast<int>(scene.drawItems.size()));
        ImGui::Separator();

        // Persistent UI state
        static int selectedNode = -1;
        static int prevSelectedNode = -2;
        static bool addAsChildOfSelection = false;

        static char nameBuf[128]{};
        static char importPathBuf[512]{};

        // Selection is driven by the main viewport (mouse picking) or by this UI.
        if (scene.editorSelectedNode != selectedNode)
        {
            selectedNode = scene.editorSelectedNode;
        }

        // Save state
        static char savePathBuf[512]{};
        static char saveStatusBuf[512]{};
        static std::string cachedSourcePath;
        static bool saveStatusIsError = false;

        // Keep save path input synced with loaded sourcePath (unless user edits it).
        if (cachedSourcePath != level.sourcePath)
        {
            cachedSourcePath = level.sourcePath;
            const std::string fallback = cachedSourcePath.empty() ? std::string("levels/edited.level.json") : cachedSourcePath;
            std::snprintf(savePathBuf, sizeof(savePathBuf), "%s", fallback.c_str());
        }

        // ------------------------------------------------------------
        // File
        // ------------------------------------------------------------
        if (ImGui::CollapsingHeader("File", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::InputText("Level path", savePathBuf, sizeof(savePathBuf));

            auto doSaveToPath = [&](const std::string& path)
            {
                try
                {
                    // Persist camera/lights from the current scene into the level asset.
                    level.camera = scene.camera;
                    level.lights = scene.lights;

                    rendern::SaveLevelAssetToJson(path, level);
                    level.sourcePath = path;
                    cachedSourcePath = path;
                    std::snprintf(saveStatusBuf, sizeof(saveStatusBuf), "Saved: %s", path.c_str());
                    saveStatusIsError = false;
                }
                catch (const std::exception& e)
                {
                    std::snprintf(saveStatusBuf, sizeof(saveStatusBuf), "Save failed: %s", e.what());
                    saveStatusIsError = true;
                }
            };

            const bool canHotkey = !ImGui::GetIO().WantTextInput;
            const bool ctrlS = canHotkey && ImGui::IsKeyDown(ImGuiKey_ModCtrl) && ImGui::IsKeyPressed(ImGuiKey_S);

            const std::string pathStr = std::string(savePathBuf);
            bool clickedSave = ImGui::Button("Save (Ctrl+S)");
            ImGui::SameLine();
            bool clickedSaveAs = ImGui::Button("Save As");

            if (ctrlS || clickedSave)
            {
                const std::string usePath = !level.sourcePath.empty() ? level.sourcePath : pathStr;
                if (!usePath.empty())
                {
                    doSaveToPath(usePath);
                }
                else
                {
                    std::snprintf(saveStatusBuf, sizeof(saveStatusBuf), "Save failed: empty path");
                    saveStatusIsError = true;
                }
            }
            else if (clickedSaveAs)
            {
                if (!pathStr.empty())
                {
                    doSaveToPath(pathStr);
                }
                else
                {
                    std::snprintf(saveStatusBuf, sizeof(saveStatusBuf), "Save failed: empty path");
                    saveStatusIsError = true;
                }
            }

            if (saveStatusBuf[0] != '\0')
            {
                if (saveStatusIsError)
                    ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", saveStatusBuf);
                else
                    ImGui::Text("%s", saveStatusBuf);
            }
        }

        // Build children adjacency (alive only)
        const std::size_t ncount = level.nodes.size();
        std::vector<std::vector<int>> children;
        children.resize(ncount);

        auto nodeAlive = [&](int idx) -> bool
        {
            if (idx < 0) 
            {
                return false;
            }
            const std::size_t i = static_cast<std::size_t>(idx);
            if (i >= ncount) 
            {
                return false;
            }
            return level.nodes[i].alive;
        };

        for (std::size_t i = 0; i < ncount; ++i)
        {
            const auto& n = level.nodes[i];
            if (!n.alive) continue;
            if (n.parent < 0) continue;
            if (!nodeAlive(n.parent)) continue;
            children[static_cast<std::size_t>(n.parent)].push_back(static_cast<int>(i));
        }

        // Roots
        std::vector<int> roots;
        roots.reserve(ncount);
        for (std::size_t i = 0; i < ncount; ++i)
        {
            const auto& n = level.nodes[i];
            if (!n.alive) continue;
            if (n.parent < 0 || !nodeAlive(n.parent))
                roots.push_back(static_cast<int>(i));
        }

        // Mesh/material id lists (sorted)
        std::vector<std::string> meshIds;
        meshIds.reserve(level.meshes.size());
        for (const auto& [id, _] : level.meshes) meshIds.push_back(id);
        std::sort(meshIds.begin(), meshIds.end());

        std::vector<std::string> materialIds;
        materialIds.reserve(level.materials.size());
        for (const auto& [id, _] : level.materials) materialIds.push_back(id);
        std::sort(materialIds.begin(), materialIds.end());

        // Layout: hierarchy + inspector
        ImGui::BeginChild("##Hierarchy", ImVec2(280.0f, 0.0f), true);

        auto drawNode = [&](auto&& self, int idx) -> void
        {
            const auto& n = level.nodes[static_cast<std::size_t>(idx)];

            ImGuiTreeNodeFlags flags =
                ImGuiTreeNodeFlags_OpenOnArrow |
                ImGuiTreeNodeFlags_SpanFullWidth;

            if (children[static_cast<std::size_t>(idx)].empty())
                flags |= ImGuiTreeNodeFlags_Leaf;

            if (idx == selectedNode)
                flags |= ImGuiTreeNodeFlags_Selected;

            char label[256]{};
            const char* name = n.name.empty() ? "<unnamed>" : n.name.c_str();
            if (!n.mesh.empty())
                std::snprintf(label, sizeof(label), "%d: %s  [mesh=%s]", idx, name, n.mesh.c_str());
            else
                std::snprintf(label, sizeof(label), "%d: %s", idx, name);

            const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<std::intptr_t>(idx)), flags, "%s", label);

            if (ImGui::IsItemClicked())
            {
                selectedNode = idx;
            }

            if (open)
            {
                for (int ch : children[static_cast<std::size_t>(idx)])
                    self(self, ch);
                ImGui::TreePop();
            }
        };

        for (int r : roots)
        {
            drawNode(drawNode, r);
        }

        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##Inspector", ImVec2(0.0f, 0.0f), true);

        // Add / import controls
        ImGui::Text("Create / Import");
        ImGui::Checkbox("Add as child of selected", &addAsChildOfSelection);

        auto ensureDefaultMesh = [&](std::string_view id, std::string_view relPath)
        {
            if (!level.meshes.contains(std::string(id)))
            {
                rendern::LevelMeshDef def{};
                def.path = std::string(relPath);
                def.debugName = std::string(id);
                level.meshes.emplace(std::string(id), std::move(def));
            }
        };

        auto computeSpawnTransform = [&]() -> rendern::Transform
        {
            rendern::Transform t{};
            t.position = scene.camera.position + camCtl.Forward() * 5.0f;
            t.rotationDegrees = mathUtils::Vec3(0.0f, 0.0f, 0.0f);
            t.scale = mathUtils::Vec3(1.0f, 1.0f, 1.0f);
            return t;
        };

        const int parentForNew =
            (addAsChildOfSelection && nodeAlive(selectedNode)) ? selectedNode : -1;

        if (ImGui::Button("Add Cube"))
        {
            ensureDefaultMesh("cube", "models/cube.obj");
            const int newIdx = levelInst.AddNode(level, scene, assets, "cube", "", parentForNew, computeSpawnTransform(), "Cube");
            selectedNode = newIdx;
        }
        ImGui::SameLine();
        if (ImGui::Button("Add Quad"))
        {
            ensureDefaultMesh("quad", "models/quad.obj");
            rendern::Transform t = computeSpawnTransform();
            t.scale = mathUtils::Vec3(3.0f, 1.0f, 3.0f);
            const int newIdx = levelInst.AddNode(level, scene, assets, "quad", "", parentForNew, t, "Quad");
            selectedNode = newIdx;
        }
        ImGui::SameLine();
        if (ImGui::Button("Add Empty"))
        {
            const int newIdx = levelInst.AddNode(level, scene, assets, "", "", parentForNew, computeSpawnTransform(), "Empty");
            selectedNode = newIdx;
        }

        ImGui::Spacing();
        ImGui::InputText("OBJ path", importPathBuf, sizeof(importPathBuf));

        auto sanitizeId = [](std::string s) -> std::string
        {
            if (s.empty())
                s = "mesh";

            for (char& c : s)
            {
                const unsigned char uc = static_cast<unsigned char>(c);
                if (!(std::isalnum(uc) || c == '_' || c == '-'))
                    c = '_';
            }
            return s;
        };

        auto makeUniqueMeshId = [&](std::string base) -> std::string
        {
            std::string id = sanitizeId(std::move(base));
            if (id.empty())
                id = "mesh";

            if (!level.meshes.contains(id))
                return id;

            for (int suffix = 2; suffix < 10000; ++suffix)
            {
                std::string tryId = id + "_" + std::to_string(suffix);
                if (!level.meshes.contains(tryId))
                    return tryId;
            }
            return id + "_x";
        };

        if (ImGui::Button("Import mesh into library"))
        {
            const std::string pathStr = std::string(importPathBuf);
            if (!pathStr.empty())
            {
                std::string base = std::filesystem::path(pathStr).stem().string();
                if (base.empty())
                    base = "mesh";

                const std::string meshId = makeUniqueMeshId(base);

                rendern::LevelMeshDef def{};
                def.path = pathStr;
                def.debugName = meshId;
                level.meshes.emplace(meshId, std::move(def));

                // Kick async load (optional)
                try
                {
                    rendern::MeshProperties p{};
                    p.filePath = pathStr;
                    p.debugName = meshId;
                    assets.LoadMeshAsync(meshId, std::move(p));
                }
                catch (...)
                {
                    // Leave it - the actual load error will be visible in logs.
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Create object from path"))
        {
            const std::string pathStr = std::string(importPathBuf);
            if (!pathStr.empty())
            {
                std::string base = std::filesystem::path(pathStr).stem().string();
                if (base.empty())
                    base = "mesh";

                const std::string meshId = makeUniqueMeshId(base);

                if (!level.meshes.contains(meshId))
                {
                    rendern::LevelMeshDef def{};
                    def.path = pathStr;
                    def.debugName = meshId;
                    level.meshes.emplace(meshId, std::move(def));
                }

                const int newIdx = levelInst.AddNode(level, scene, assets, meshId, "", parentForNew, computeSpawnTransform(), meshId);
                selectedNode = newIdx;
            }
        }

        ImGui::Separator();
        ImGui::Text("Selection");

        // Selection validity
        if (selectedNode >= 0 && (!nodeAlive(selectedNode)))
        {
            selectedNode = -1;
        }

        if (selectedNode >= 0 && nodeAlive(selectedNode))
        {
            rendern::LevelNode& node = level.nodes[static_cast<std::size_t>(selectedNode)];

            if (prevSelectedNode != selectedNode)
            {
                // Refresh name buffer on selection change.
                std::snprintf(nameBuf, sizeof(nameBuf), "%s", node.name.c_str());
                prevSelectedNode = selectedNode;
            }

            ImGui::Text("Node #%d", selectedNode);

            if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf)))
            {
                node.name = std::string(nameBuf);
            }

            bool vis = node.visible;
            if (ImGui::Checkbox("Visible", &vis))
            {
                levelInst.SetNodeVisible(level, scene, assets, selectedNode, vis);
            }

            // Mesh combo
            {
                std::vector<std::string> items;
                items.reserve(meshIds.size() + 2);
                items.push_back("(none)");
                for (const auto& id : meshIds) items.push_back(id);

                // Ensure missing mesh id is still selectable (shows as last item)
                if (!node.mesh.empty() && !level.meshes.contains(node.mesh))
                    items.push_back(std::string("<missing> ") + node.mesh);

                int current = 0;
                if (!node.mesh.empty())
                {
                    for (std::size_t i = 1; i < items.size(); ++i)
                    {
                        if (items[i] == node.mesh)
                        {
                            current = static_cast<int>(i);
                            break;
                        }
                    }
                }

                std::vector<const char*> citems;
                citems.reserve(items.size());
                for (auto& s : items) citems.push_back(s.c_str());

                if (ImGui::Combo("Mesh", &current, citems.data(), static_cast<int>(citems.size())))
                {
                    if (current == 0)
                        levelInst.SetNodeMesh(level, scene, assets, selectedNode, "");
                    else
                        levelInst.SetNodeMesh(level, scene, assets, selectedNode, items[static_cast<std::size_t>(current)]);
                }
            }

            // Material combo
            {
                std::vector<std::string> items;
                items.reserve(materialIds.size() + 2);
                items.push_back("(none)");
                for (const auto& id : materialIds) items.push_back(id);

                if (!node.material.empty() && !level.materials.contains(node.material))
                    items.push_back(std::string("<missing> ") + node.material);

                int current = 0;
                if (!node.material.empty())
                {
                    for (std::size_t i = 1; i < items.size(); ++i)
                    {
                        if (items[i] == node.material)
                        {
                            current = static_cast<int>(i);
                            break;
                        }
                    }
                }

                std::vector<const char*> citems;
                citems.reserve(items.size());
                for (auto& s : items) citems.push_back(s.c_str());

                if (ImGui::Combo("Material", &current, citems.data(), static_cast<int>(citems.size())))
                {
                    if (current == 0)
                        levelInst.SetNodeMaterial(level, scene, selectedNode, "");
                    else
                        levelInst.SetNodeMaterial(level, scene, selectedNode, items[static_cast<std::size_t>(current)]);
                }
            }

            // Transform
            bool changed = false;
            changed |= DragVec3("Position", node.transform.position, 0.05f);
            changed |= DragVec3("Rotation (deg)", node.transform.rotationDegrees, 0.2f);

            mathUtils::Vec3 scale = node.transform.scale;
            if (DragVec3("Scale", scale, 0.02f))
            {
                scale.x = std::max(scale.x, 0.001f);
                scale.y = std::max(scale.y, 0.001f);
                scale.z = std::max(scale.z, 0.001f);
                node.transform.scale = scale;
                changed = true;
            }

            if (changed)
            {
                levelInst.MarkTransformsDirty();
            }

            ImGui::Spacing();

            // Actions
            if (ImGui::Button("Duplicate"))
            {
                rendern::Transform t = node.transform;
                t.position.x += 1.0f;

                const int newIdx = levelInst.AddNode(level, scene, assets, node.mesh, node.material, node.parent, t, node.name);
                selectedNode = newIdx;
            }
            ImGui::SameLine();
            bool doDelete = ImGui::Button("Delete (recursive)");
            if (ImGui::IsKeyPressed(ImGuiKey_Delete))
            {
                doDelete = true;
            }

            if (doDelete)
            {
                const int parent = node.parent;
                levelInst.DeleteSubtree(level, scene, selectedNode);

                if (nodeAlive(parent))
                {    
                    selectedNode = parent;
                }
                else
                {
                    selectedNode = -1;
                }
            }
        }
        else
        {
            ImGui::TextDisabled("No node selected.");
            prevSelectedNode = -2;
        }

        ImGui::EndChild();

        // Expose selection to the rest of the app (main viewport already writes here too).
        scene.editorSelectedNode = selectedNode;

        // Push transforms to Scene if needed
        levelInst.SyncTransformsIfDirty(level, scene);

        ImGui::End();
    }

}
