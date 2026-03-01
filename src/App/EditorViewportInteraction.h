#pragma once

namespace appEditor
{
    struct EditorViewportInteraction
    {
        rendern::TranslateGizmoController translateGizmo{};
        rendern::RotateGizmoController rotateGizmo{};
        rendern::ScaleGizmoController scaleGizmo{};
    };

    template <typename TGizmoState>
    inline void ResetGizmoState(TGizmoState& gizmo)
    {
        gizmo.visible = false;
        gizmo.hoveredAxis = rendern::GizmoAxis::None;
        gizmo.activeAxis = rendern::GizmoAxis::None;
    }

    inline void ClearAllGizmoHover(EditorViewportInteraction& interaction, rendern::Scene& scene)
    {
        interaction.translateGizmo.ClearHover(scene);
        interaction.rotateGizmo.ClearHover(scene);
        interaction.scaleGizmo.ClearHover(scene);
    }

    inline void EndAllGizmoDrags(EditorViewportInteraction& interaction, rendern::Scene& scene)
    {
        interaction.translateGizmo.EndDrag(scene);
        interaction.rotateGizmo.EndDrag(scene);
        interaction.scaleGizmo.EndDrag(scene);
    }

    inline void SyncCurrentGizmoVisual(
        EditorViewportInteraction& interaction,
        rendern::LevelAsset& levelAsset,
        rendern::LevelInstance& levelInstance,
        rendern::Scene& scene)
    {
        if (scene.editorGizmoMode == rendern::GizmoMode::Translate)
        {
            interaction.translateGizmo.SyncVisual(levelAsset, levelInstance, scene);
        }
        else if (scene.editorGizmoMode == rendern::GizmoMode::Rotate)
        {
            interaction.rotateGizmo.SyncVisual(levelAsset, levelInstance, scene);
        }
        else if (scene.editorGizmoMode == rendern::GizmoMode::Scale)
        {
            interaction.scaleGizmo.SyncVisual(levelAsset, levelInstance, scene);
        }
    }

    inline void ApplyGizmoModeHotkeys(
        EditorViewportInteraction& interaction,
        rendern::Scene& scene,
        const rendern::InputState& input)
    {
        if (!input.hasFocus || input.capture.captureKeyboard || input.mouse.rmbDown)
        {
            return;
        }

        rendern::GizmoMode nextMode = scene.editorGizmoMode;
        if (input.KeyPressed('Q'))
        {
            nextMode = rendern::GizmoMode::None;
        }
        else if (input.KeyPressed('W'))
        {
            nextMode = rendern::GizmoMode::Translate;
        }
        else if (input.KeyPressed('E'))
        {
            nextMode = rendern::GizmoMode::Rotate;
        }
        else if (input.KeyPressed('R'))
        {
            nextMode = rendern::GizmoMode::Scale;
        }

        if (nextMode != scene.editorGizmoMode)
        {
            EndAllGizmoDrags(interaction, scene);
            scene.editorGizmoMode = nextMode;
        }
    }

    inline void SyncEditorGizmoVisuals(
        EditorViewportInteraction& interaction,
        rendern::LevelAsset& levelAsset,
        rendern::LevelInstance& levelInstance,
        rendern::Scene& scene)
    {
        levelInstance.SyncTransformsIfDirty(levelAsset, scene);

        if (scene.editorGizmoMode == rendern::GizmoMode::Translate)
        {
            interaction.translateGizmo.SyncVisual(levelAsset, levelInstance, scene);
            ResetGizmoState(scene.editorRotateGizmo);
            ResetGizmoState(scene.editorScaleGizmo);
        }
        else if (scene.editorGizmoMode == rendern::GizmoMode::Rotate)
        {
            ResetGizmoState(scene.editorTranslateGizmo);
            ResetGizmoState(scene.editorScaleGizmo);
            interaction.rotateGizmo.SyncVisual(levelAsset, levelInstance, scene);
        }
        else if (scene.editorGizmoMode == rendern::GizmoMode::Scale)
        {
            ResetGizmoState(scene.editorTranslateGizmo);
            ResetGizmoState(scene.editorRotateGizmo);
            interaction.scaleGizmo.SyncVisual(levelAsset, levelInstance, scene);
        }
        else
        {
            ResetGizmoState(scene.editorTranslateGizmo);
            ResetGizmoState(scene.editorRotateGizmo);
            ResetGizmoState(scene.editorScaleGizmo);
        }
    }

    inline bool TryGetViewportMouseClientPos(HWND hwnd, int viewportWidth, int viewportHeight, int& outMouseX, int& outMouseY)
    {
        POINT point{};
        if (!GetCursorPos(&point) || !ScreenToClient(hwnd, &point))
        {
            return false;
        }

        if (point.x < 0 || point.y < 0 || point.x >= viewportWidth || point.y >= viewportHeight)
        {
            return false;
        }

        outMouseX = point.x;
        outMouseY = point.y;
        return true;
    }

    inline void UpdateViewportGizmoHover(
        EditorViewportInteraction& interaction,
        HWND hwnd,
        int viewportWidth,
        int viewportHeight,
        rendern::Scene& scene,
        const rendern::InputState& input)
    {
        if (!input.hasFocus || input.mouse.rmbDown || input.capture.captureMouse)
        {
            ClearAllGizmoHover(interaction, scene);
            return;
        }

        int mouseX = 0;
        int mouseY = 0;
        if (!TryGetViewportMouseClientPos(hwnd, viewportWidth, viewportHeight, mouseX, mouseY))
        {
            ClearAllGizmoHover(interaction, scene);
            return;
        }

        const float mouseXF = static_cast<float>(mouseX);
        const float mouseYF = static_cast<float>(mouseY);
        const float viewportWidthF = static_cast<float>(viewportWidth);
        const float viewportHeightF = static_cast<float>(viewportHeight);

        if (scene.editorGizmoMode == rendern::GizmoMode::Translate)
        {
            interaction.translateGizmo.UpdateHover(scene, mouseXF, mouseYF, viewportWidthF, viewportHeightF);
            interaction.rotateGizmo.ClearHover(scene);
            interaction.scaleGizmo.ClearHover(scene);
        }
        else if (scene.editorGizmoMode == rendern::GizmoMode::Rotate)
        {
            interaction.rotateGizmo.UpdateHover(scene, mouseXF, mouseYF, viewportWidthF, viewportHeightF);
            interaction.translateGizmo.ClearHover(scene);
            interaction.scaleGizmo.ClearHover(scene);
        }
        else if (scene.editorGizmoMode == rendern::GizmoMode::Scale)
        {
            interaction.scaleGizmo.UpdateHover(scene, mouseXF, mouseYF, viewportWidthF, viewportHeightF);
            interaction.translateGizmo.ClearHover(scene);
            interaction.rotateGizmo.ClearHover(scene);
        }
        else
        {
            ClearAllGizmoHover(interaction, scene);
        }
    }

    inline void SyncTransformsAndCurrentGizmoVisual(
        EditorViewportInteraction& interaction,
        rendern::LevelAsset& levelAsset,
        rendern::LevelInstance& levelInstance,
        rendern::Scene& scene)
    {
        levelInstance.MarkTransformsDirty();
        levelInstance.SyncTransformsIfDirty(levelAsset, scene);
        SyncCurrentGizmoVisual(interaction, levelAsset, levelInstance, scene);
    }

    inline void HandleViewportMouseInteraction(
        EditorViewportInteraction& interaction,
        HWND hwnd,
        int viewportWidth,
        int viewportHeight,
        rendern::LevelAsset& levelAsset,
        rendern::LevelInstance& levelInstance,
        rendern::Scene& scene,
        const rendern::InputState& input)
    {
        if (!input.hasFocus || input.mouse.rmbDown || input.capture.captureMouse)
        {
            if (!input.KeyDown(VK_LBUTTON))
            {
                EndAllGizmoDrags(interaction, scene);
            }
            return;
        }

        POINT point{};
        if (!GetCursorPos(&point) || !ScreenToClient(hwnd, &point))
        {
            return;
        }

        const int mouseX = point.x;
        const int mouseY = point.y;
        if (mouseX < 0 || mouseY < 0 || mouseX >= viewportWidth || mouseY >= viewportHeight)
        {
            if (!input.KeyDown(VK_LBUTTON))
            {
                EndAllGizmoDrags(interaction, scene);
            }
            return;
        }

        const float mouseXF = static_cast<float>(mouseX);
        const float mouseYF = static_cast<float>(mouseY);
        const float viewportWidthF = static_cast<float>(viewportWidth);
        const float viewportHeightF = static_cast<float>(viewportHeight);

        bool gizmoConsumed = false;
        bool transformChanged = false;

        if (scene.editorGizmoMode == rendern::GizmoMode::Translate)
        {
            if (interaction.translateGizmo.IsDragging())
            {
                if (input.KeyDown(VK_LBUTTON))
                {
                    transformChanged = interaction.translateGizmo.UpdateDrag(levelAsset, levelInstance, scene, mouseXF, mouseYF, viewportWidthF, viewportHeightF, input.shiftDown);
                    gizmoConsumed = true;
                }
                else
                {
                    interaction.translateGizmo.EndDrag(scene);
                    gizmoConsumed = true;
                }
            }
            else if (input.KeyPressed(VK_LBUTTON))
            {
                gizmoConsumed = interaction.translateGizmo.TryBeginDrag(levelAsset, levelInstance, scene, mouseXF, mouseYF, viewportWidthF, viewportHeightF);
            }
        }
        else if (scene.editorGizmoMode == rendern::GizmoMode::Rotate)
        {
            if (interaction.rotateGizmo.IsDragging())
            {
                if (input.KeyDown(VK_LBUTTON))
                {
                    transformChanged = interaction.rotateGizmo.UpdateDrag(levelAsset, levelInstance, scene, mouseXF, mouseYF, viewportWidthF, viewportHeightF, input.shiftDown);
                    gizmoConsumed = true;
                }
                else
                {
                    interaction.rotateGizmo.EndDrag(scene);
                    gizmoConsumed = true;
                }
            }
            else if (input.KeyPressed(VK_LBUTTON))
            {
                gizmoConsumed = interaction.rotateGizmo.TryBeginDrag(levelAsset, levelInstance, scene, mouseXF, mouseYF, viewportWidthF, viewportHeightF);
            }
        }
        else if (scene.editorGizmoMode == rendern::GizmoMode::Scale)
        {
            if (interaction.scaleGizmo.IsDragging())
            {
                if (input.KeyDown(VK_LBUTTON))
                {
                    transformChanged = interaction.scaleGizmo.UpdateDrag(levelAsset, levelInstance, scene, mouseXF, mouseYF, viewportWidthF, viewportHeightF, input.shiftDown);
                    gizmoConsumed = true;
                }
                else
                {
                    interaction.scaleGizmo.EndDrag(scene);
                    gizmoConsumed = true;
                }
            }
            else if (input.KeyPressed(VK_LBUTTON))
            {
                gizmoConsumed = interaction.scaleGizmo.TryBeginDrag(levelAsset, levelInstance, scene, mouseXF, mouseYF, viewportWidthF, viewportHeightF);
            }
        }

        if (transformChanged)
        {
            SyncTransformsAndCurrentGizmoVisual(interaction, levelAsset, levelInstance, scene);
        }

        if (!gizmoConsumed && input.KeyPressed(VK_LBUTTON))
        {
            const rendern::PickResult pick = rendern::PickNodeUnderScreenPoint(
                scene,
                levelInstance,
                mouseXF,
                mouseYF,
                viewportWidthF,
                viewportHeightF);

            scene.debugPickRay.enabled = true;
            scene.debugPickRay.origin = pick.rayOrigin;
            scene.debugPickRay.direction = pick.rayDir;
            scene.debugPickRay.hit = (pick.nodeIndex >= 0) && std::isfinite(pick.t);
            scene.debugPickRay.length = scene.debugPickRay.hit ? pick.t : scene.camera.farZ;

            if (scene.debugPickRay.hit && levelInstance.IsNodeAlive(levelAsset, pick.nodeIndex))
            {
                scene.editorSelectedNode = pick.nodeIndex;
                scene.editorSelectedDrawItem = levelInstance.GetNodeDrawIndex(pick.nodeIndex);
            }
            else
            {
                scene.editorSelectedNode = -1;
                scene.editorSelectedDrawItem = -1;
            }
        }
    }
} // namespace appEditor
