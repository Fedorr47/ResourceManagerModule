namespace rendern::ui
{
    static void DrawSSAOSection(rendern::RendererSettings& rs)
    {
        if (!rs.enableDeferred)
            return;

        if (!ImGui::CollapsingHeader("SSAO", ImGuiTreeNodeFlags_DefaultOpen))
            return;

        ImGui::Checkbox("Enable SSAO", &rs.enableSSAO);
        ImGui::BeginDisabled(!rs.enableSSAO);

        ImGui::SliderFloat("Radius", &rs.ssaoRadius, 0.05f, 5.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SliderFloat("Bias", &rs.ssaoBias, 0.0f, 0.25f, "%.4f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SliderFloat("Strength", &rs.ssaoStrength, 0.0f, 4.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SliderFloat("Power", &rs.ssaoPower, 0.5f, 4.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SliderFloat("Blur depth threshold", &rs.ssaoBlurDepthThreshold, 0.0f, 0.02f, "%.5f", ImGuiSliderFlags_AlwaysClamp);

        if (ImGui::Button("SSAO defaults"))
        {
            rs.ssaoRadius = 1.0f;
            rs.ssaoBias = 0.02f;
            rs.ssaoStrength = 1.25f;
            rs.ssaoPower = 1.5f;
            rs.ssaoBlurDepthThreshold = 0.0025f;
        }

        ImGui::EndDisabled();
        ImGui::Separator();
    }

    static void DrawFogSection(rendern::RendererSettings& rs)
    {
        if (!ImGui::CollapsingHeader("Fog", ImGuiTreeNodeFlags_DefaultOpen))
            return;

        ImGui::Checkbox("Enable fog", &rs.enableFog);
        ImGui::BeginDisabled(!rs.enableFog);

        const char* items[] = { "Linear", "Exp", "Exp2" };
        int mode = static_cast<int>(rs.fogMode);
        if (ImGui::Combo("Mode", &mode, items, IM_ARRAYSIZE(items)))
        {
            if (mode < 0) mode = 0;
            if (mode > 2) mode = 2;
            rs.fogMode = static_cast<std::uint32_t>(mode);
        }

        if (rs.fogMode == 0u)
        {
            ImGui::SliderFloat("Start", &rs.fogStart, 0.0f, 500.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SliderFloat("End", &rs.fogEnd, 0.0f, 500.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);

            if (rs.fogEnd < rs.fogStart)
            {
                std::swap(rs.fogEnd, rs.fogStart);
            }
        }
        else
        {
            ImGui::SliderFloat("Density", &rs.fogDensity, 0.0f, 0.25f, "%.4f", ImGuiSliderFlags_AlwaysClamp);
        }

        ImGui::ColorEdit3("Color", rs.fogColor.data());

        if (ImGui::Button("Fog defaults"))
        {
            rs.fogMode = 0u;
            rs.fogStart = 15.0f;
            rs.fogEnd = 80.0f;
            rs.fogDensity = 0.02f;
            rs.fogColor = { 0.60f, 0.70f, 0.80f };
        }

        ImGui::EndDisabled();
        ImGui::Separator();
    }


    static void DrawHdrBloomSection(rendern::RendererSettings& rs)
    {
        if (!ImGui::CollapsingHeader("HDR / Bloom", ImGuiTreeNodeFlags_DefaultOpen))
            return;

        ImGui::Checkbox("Enable HDR", &rs.enableHDR);
        ImGui::BeginDisabled(!rs.enableHDR);

        const char* toneItems[] = { "Linear", "Reinhard", "ACES" };
        int toneMode = static_cast<int>(rs.toneMapMode);
        if (ImGui::Combo("Tonemap", &toneMode, toneItems, IM_ARRAYSIZE(toneItems)))
        {
            if (toneMode < 0) toneMode = 0;
            if (toneMode > 2) toneMode = 2;
            rs.toneMapMode = static_cast<std::uint32_t>(toneMode);
        }

        ImGui::SliderFloat("Exposure", &rs.hdrExposure, 0.1f, 8.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::Checkbox("Enable Bloom", &rs.enableBloom);

        ImGui::BeginDisabled(!rs.enableBloom);
        ImGui::SliderFloat("Bloom threshold", &rs.bloomThreshold, 0.1f, 8.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SliderFloat("Bloom soft knee", &rs.bloomSoftKnee, 0.0f, 2.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SliderFloat("Bloom intensity", &rs.bloomIntensity, 0.0f, 1.5f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SliderFloat("Bloom clamp", &rs.bloomClamp, 1.0f, 64.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SliderFloat("Bloom radius", &rs.bloomRadius, 0.25f, 4.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::EndDisabled();

        if (ImGui::Button("HDR/Bloom defaults"))
        {
            rs.enableHDR = true;
            rs.toneMapMode = 2u;
            rs.hdrExposure = 1.0f;
            rs.enableBloom = true;
            rs.bloomThreshold = 1.0f;
            rs.bloomSoftKnee = 0.5f;
            rs.bloomIntensity = 0.08f;
            rs.bloomClamp = 16.0f;
            rs.bloomRadius = 1.0f;
        }

        ImGui::EndDisabled();
        ImGui::Separator();
    }

    static void DrawCameraDebugSection(rendern::Scene& scene, rendern::CameraController& camCtl)
    {
        if (!ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen))
            return;

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
    }

    static void DrawShadowAndDebugSection(rendern::RendererSettings& rs, rendern::Scene& scene)
    {
        int current = static_cast<int>(rs.debugShadowCubeMapType);
        std::vector<const char*> citems;
        citems.reserve(2);
        citems.push_back("Point");
        citems.push_back("Reflection");

        ImGui::Separator();
        ImGui::Text("Shadow cube atlas");
        ImGui::Checkbox("Show cube atlas", &rs.ShowCubeAtlas);

        int debugCubeAtlasIndex = static_cast<int>(rs.debugCubeAtlasIndex);
        if (ImGui::Combo("Type", &current, citems.data(), static_cast<int>(citems.size())))
        {
            rs.debugShadowCubeMapType = static_cast<std::uint32_t>(current);
        }

        if (current == 1)
        {
            int reflectiveOwnerCount = 0;
            for (const auto& di : scene.drawItems)
            {
                if (di.material.id == 0)
                    continue;
                const auto& mat = scene.GetMaterial(di.material);
                if (mat.envSource == EnvSource::ReflectionCapture)
                    ++reflectiveOwnerCount;
            }

            ImGui::TextDisabled("Reflection owner index among reflective objects (count: %d)", reflectiveOwnerCount);
            ImGui::TextDisabled("Debug atlas index now selects which reflective owner is captured/shown.");
            if (scene.editorReflectionCaptureOwnerNode >= 0)
            {
                ImGui::TextDisabled("In reflection atlas debug mode, the debug owner index overrides the explicit capture owner.");
            }
        }

        const char* debugIndexLabel = (current == 0) ? "Point cube index" : "Reflection owner index";
        if (ImGui::InputInt(debugIndexLabel, &debugCubeAtlasIndex))
        {
            if (debugCubeAtlasIndex < 0)
            {
                debugCubeAtlasIndex = 0;
            }
            rs.debugCubeAtlasIndex = static_cast<std::uint32_t>(debugCubeAtlasIndex);
        }

        ImGui::Separator();
        ImGui::Text("Shadow bias (texels)");
        ImGui::SliderFloat("Dir base", &rs.dirShadowBaseBiasTexels, 0.0f, 5.0f, "%.3f");
        ImGui::SliderFloat("Spot base", &rs.spotShadowBaseBiasTexels, 0.0f, 10.0f, "%.3f");
        ImGui::SliderFloat("Point base", &rs.pointShadowBaseBiasTexels, 0.0f, 10.0f, "%.3f");
        ImGui::SliderFloat("Slope scale", &rs.shadowSlopeScaleTexels, 0.0f, 10.0f, "%.3f");

        ImGui::Separator();
        ImGui::Text("Debug draw");
        ImGui::Checkbox("Light gizmos", &rs.drawLightGizmos);
        ImGui::Checkbox("Planar mirror normals", &rs.drawPlanarMirrorNormals);
        if (rs.drawPlanarMirrorNormals)
        {
            ImGui::SliderFloat("Planar normal length", &rs.planarMirrorNormalLength, 0.05f, 20.0f, "%.3f");
        }
        ImGui::BeginDisabled(!rs.drawLightGizmos);
        ImGui::Checkbox("Depth test (main view)", &rs.debugDrawDepthTest);
        ImGui::SliderFloat("Gizmo half-size", &rs.lightGizmoHalfSize, 0.01f, 2.0f, "%.3f");
        ImGui::SliderFloat("Arrow length", &rs.lightGizmoArrowLength, 0.05f, 25.0f, "%.3f");
        ImGui::SliderFloat("Arrow thickness (UI only)", &rs.lightGizmoArrowThickness, 0.001f, 2.0f, "%.3f");
        ImGui::EndDisabled();
    }

    static void DrawRendererCoreWindow(
        rendern::RendererSettings& rs,
        rendern::Scene& scene,
        rendern::CameraController& camCtl)
    {
        ImGui::Begin("Renderer / Shadows");

        ImGui::Checkbox("Depth prepass", &rs.enableDepthPrepass);
        ImGui::Checkbox("Deferred (experimental)", &rs.enableDeferred);
        ImGui::Checkbox("Frustum culling", &rs.enableFrustumCulling);
        ImGui::Checkbox("Debug print draw calls", &rs.debugPrintDrawCalls);

        DrawSSAOSection(rs);
        DrawFogSection(rs);
        DrawHdrBloomSection(rs);

        DrawCameraDebugSection(scene, camCtl);
        DrawShadowAndDebugSection(rs, scene);

        ImGui::Separator();
        ImGui::TextDisabled("F1: toggle UI");
        ImGui::End();
    }
}
