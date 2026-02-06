module;

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

export namespace rendern::ui
{
    void DrawRendererDebugUI(rendern::RendererSettings& rs, rendern::Scene& scene, rendern::CameraController& camCtl);
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

        static bool ProjectWorldToScreen(
            const mathUtils::Vec3& worldPosition,
            const mathUtils::Mat4& viewProj,
            const ImVec2& displaySize,
            ImVec2& outScreen)
        {
            const mathUtils::Vec4 clip = viewProj * mathUtils::Vec4(worldPosition, 1.0f);

            if (clip.w <= 0.00001f)
            {
                return false;
            }

            const float invW = 1.0f / clip.w;
            const float ndcX = clip.x * invW;
            const float ndcY = clip.y * invW;

            // optionally cull points outside screen
            if (ndcX < -1.2f || ndcX > 1.2f || ndcY < -1.2f || ndcY > 1.2f)
            {
                return false;
            }

            const float screenX = (ndcX * 0.5f + 0.5f) * displaySize.x;
            const float screenY = (1.0f - (ndcY * 0.5f + 0.5f)) * displaySize.y; // flip Y

            outScreen = ImVec2(screenX, screenY);
            return true;
        }

        static void DrawLightGizmosOverlay(const rendern::Scene& scene, const rendern::RendererSettings& rendererSettings)
        {
            const ImGuiIO& io = ImGui::GetIO();
            const ImVec2 displaySize = io.DisplaySize;

            const float aspect = (displaySize.y > 0.0f) ? (displaySize.x / displaySize.y) : 1.0f;

            const mathUtils::Mat4 projection =
                mathUtils::PerspectiveRH_ZO(mathUtils::DegToRad(scene.camera.fovYDeg), aspect, scene.camera.nearZ, scene.camera.farZ);

            const mathUtils::Mat4 view =
                mathUtils::LookAt(scene.camera.position, scene.camera.target, scene.camera.up);

            const mathUtils::Mat4 viewProj = projection * view;

            ImDrawList* drawList = ImGui::GetForegroundDrawList();
            if (!drawList)
            {
                return;
            }

            const float pointRadius = 6.0f * rendererSettings.debugLightGizmoScale;
            const float arrowLength = rendererSettings.lightGizmoArrowLength * rendererSettings.debugLightGizmoScale;

            auto DrawArrow = [&](const mathUtils::Vec3& worldStart, const mathUtils::Vec3& worldEnd, ImU32 color)
                {
                    ImVec2 startScreen{};
                    ImVec2 endScreen{};

                    if (!ProjectWorldToScreen(worldStart, viewProj, displaySize, startScreen))
                    {
                        return;
                    }
                    if (!ProjectWorldToScreen(worldEnd, viewProj, displaySize, endScreen))
                    {
                        return;
                    }

                    drawList->AddLine(startScreen, endScreen, color, 2.0f);

                    // simple arrow head (screen-space)
                    const ImVec2 dir = ImVec2(endScreen.x - startScreen.x, endScreen.y - startScreen.y);
                    const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
                    if (len > 1.0f)
                    {
                        const ImVec2 unit = ImVec2(dir.x / len, dir.y / len);
                        const ImVec2 left = ImVec2(-unit.y, unit.x);

                        const float headSize = 8.0f * rendererSettings.debugLightGizmoScale;
                        const ImVec2 p0 = endScreen;
                        const ImVec2 p1 = ImVec2(endScreen.x - unit.x * headSize + left.x * headSize * 0.6f,
                            endScreen.y - unit.y * headSize + left.y * headSize * 0.6f);
                        const ImVec2 p2 = ImVec2(endScreen.x - unit.x * headSize - left.x * headSize * 0.6f,
                            endScreen.y - unit.y * headSize - left.y * headSize * 0.6f);

                        drawList->AddTriangleFilled(p0, p1, p2, color);
                    }
                };

            for (std::size_t lightIndex = 0; lightIndex < scene.lights.size(); ++lightIndex)
            {
                const rendern::Light& light = scene.lights[lightIndex];

                ImU32 color = IM_COL32(255, 255, 255, 255);
                if (light.type == rendern::LightType::Directional) { color = IM_COL32(255, 230, 120, 255); }
                if (light.type == rendern::LightType::Point) { color = IM_COL32(120, 255, 120, 255); }
                if (light.type == rendern::LightType::Spot) { color = IM_COL32(120, 180, 255, 255); }

                if (light.type == rendern::LightType::Directional)
                {
                    // directional has no position -> draw at camera target (or center)
                    const mathUtils::Vec3 anchor = scene.camera.target;
                    const mathUtils::Vec3 directionFromLight = mathUtils::Normalize(light.direction); // your convention: FROM light
                    const mathUtils::Vec3 arrowEnd = anchor + directionFromLight * arrowLength;

                    DrawArrow(anchor, arrowEnd, color);

                    ImVec2 labelPos{};
                    if (ProjectWorldToScreen(anchor, viewProj, displaySize, labelPos))
                    {
                        drawList->AddText(labelPos, color, "Dir");
                    }

                    continue;
                }

                // point / spot: draw position marker
                ImVec2 lightScreen{};
                if (ProjectWorldToScreen(light.position, viewProj, displaySize, lightScreen))
                {
                    drawList->AddCircleFilled(lightScreen, pointRadius, color);
                    drawList->AddCircle(lightScreen, pointRadius + 2.0f, IM_COL32(0, 0, 0, 180), 0, 2.0f);

                    char label[32]{};
                    if (light.type == rendern::LightType::Point)
                    {
                        std::snprintf(label, sizeof(label), "P%zu", lightIndex);
                    }
                    else
                    {
                        std::snprintf(label, sizeof(label), "S%zu", lightIndex);
                    }
                    drawList->AddText(ImVec2(lightScreen.x + 8.0f, lightScreen.y - 10.0f), color, label);
                }

                if (light.type == rendern::LightType::Spot)
                {
                    const mathUtils::Vec3 directionFromLight = mathUtils::Normalize(light.direction); // FROM light
                    const mathUtils::Vec3 arrowEnd = light.position + directionFromLight * arrowLength;
                    DrawArrow(light.position, arrowEnd, color);
                }
            }
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
        if (rs.drawLightGizmos)
        {
            ImGui::SliderFloat("Gizmo half-size", &rs.lightGizmoHalfSize, 0.01f, 2.0f, "%.3f");
            ImGui::SliderFloat("Arrow length", &rs.lightGizmoArrowLength, 0.05f, 25.0f, "%.3f");
            ImGui::SliderFloat("Arrow thickness", &rs.lightGizmoArrowThickness, 0.001f, 2.0f, "%.3f");
            DrawLightGizmosOverlay(scene, rs);
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
}
