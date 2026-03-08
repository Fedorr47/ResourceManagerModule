#pragma once

namespace appEditor
{
    inline bool HasParticleEmitterSelection(const rendern::Scene& scene)
    {
        return scene.editorSelectedParticleEmitter >= 0 && scene.editorSelectedNode < 0;
    }

    inline void SyncSelectedParticleEmitterTranslateGizmo(
        rendern::LevelAsset& levelAsset,
        rendern::Scene& scene)
    {
        if (!HasParticleEmitterSelection(scene) ||
            static_cast<std::size_t>(scene.editorSelectedParticleEmitter) >= levelAsset.particleEmitters.size())
        {
            scene.editorTranslateGizmo.visible = false;
            scene.editorTranslateGizmo.hoveredAxis = rendern::GizmoAxis::None;
            scene.editorTranslateGizmo.activeAxis = rendern::GizmoAxis::None;
            return;
        }

        const rendern::ParticleEmitter& emitter = levelAsset.particleEmitters[static_cast<std::size_t>(scene.editorSelectedParticleEmitter)];
        auto& gizmo = scene.editorTranslateGizmo;
        gizmo.visible = gizmo.enabled;
        gizmo.pivotWorld = emitter.position;
        const float distToCamera = mathUtils::Length(scene.camera.position - gizmo.pivotWorld);
        gizmo.axisLengthWorld = std::clamp(distToCamera * 0.18f, 0.35f, 4.0f);
        gizmo.axisXWorld = mathUtils::Vec3(1.0f, 0.0f, 0.0f);
        gizmo.axisYWorld = mathUtils::Vec3(0.0f, 1.0f, 0.0f);
        gizmo.axisZWorld = mathUtils::Vec3(0.0f, 0.0f, 1.0f);
    }

    inline bool TryBeginParticleEmitterTranslateDrag(
        rendern::LevelAsset& levelAsset,
        rendern::Scene& scene,
        float mouseX,
        float mouseY,
        float viewportW,
        float viewportH)
    {
        if (!HasParticleEmitterSelection(scene))
        {
            return false;
        }

        auto& gizmo = scene.editorTranslateGizmo;
        const rendern::GizmoAxis axis = rendern::HitTestTranslateGizmoAxis(scene, gizmo, mouseX, mouseY, viewportW, viewportH);
        if (axis == rendern::GizmoAxis::None)
        {
            return false;
        }

        const geometry::Ray ray = rendern::BuildMouseRay(scene, mouseX, mouseY, viewportW, viewportH);
        const mathUtils::Vec3 axisWorld = rendern::TranslateGizmoAxisDirection(gizmo, axis);
        mathUtils::Vec3 viewDir = scene.camera.target - scene.camera.position;
        if (mathUtils::Length(viewDir) <= 1e-5f)
        {
            viewDir = mathUtils::Vec3(0.0f, 0.0f, -1.0f);
        }
        else
        {
            viewDir = mathUtils::Normalize(viewDir);
        }
        const mathUtils::Vec3 planeNormal = rendern::ComputeTranslateGizmoAxisDragPlaneNormal(axisWorld, viewDir);

        mathUtils::Vec3 startHit{};
        if (!rendern::IntersectRayPlaneForGizmo(ray, gizmo.pivotWorld, planeNormal, startHit))
        {
            return false;
        }

        auto& drag = scene.editorParticleEmitterTranslateDrag;
        drag.dragging = true;
        drag.activeAxis = axis;
        drag.dragStartWorldHit = startHit;
        drag.dragStartEmitterPosition = levelAsset.particleEmitters[static_cast<std::size_t>(scene.editorSelectedParticleEmitter)].position;
        drag.dragPlaneNormal = planeNormal;
        drag.dragAxisWorld = axisWorld;
        gizmo.activeAxis = axis;
        gizmo.hoveredAxis = axis;
        return true;
    }

    inline bool UpdateParticleEmitterTranslateDrag(
        rendern::LevelAsset& levelAsset,
        rendern::LevelInstance& levelInstance,
        rendern::Scene& scene,
        float mouseX,
        float mouseY,
        float viewportW,
        float viewportH)
    {
        auto& drag = scene.editorParticleEmitterTranslateDrag;
        if (!drag.dragging || !HasParticleEmitterSelection(scene))
        {
            return false;
        }

        const geometry::Ray ray = rendern::BuildMouseRay(scene, mouseX, mouseY, viewportW, viewportH);
        mathUtils::Vec3 currentHit{};
        if (!rendern::IntersectRayPlaneForGizmo(ray, scene.editorTranslateGizmo.pivotWorld, drag.dragPlaneNormal, currentHit))
        {
            return false;
        }

        const float axisDelta = mathUtils::Dot(currentHit - drag.dragStartWorldHit, drag.dragAxisWorld);
        const mathUtils::Vec3 newPos = drag.dragStartEmitterPosition + drag.dragAxisWorld * axisDelta;
        levelAsset.particleEmitters[static_cast<std::size_t>(scene.editorSelectedParticleEmitter)].position = newPos;
        levelInstance.SetParticleEmitterPosition(levelAsset, scene, scene.editorSelectedParticleEmitter, newPos);
        SyncSelectedParticleEmitterTranslateGizmo(levelAsset, scene);
        return true;
    }

    inline void EndParticleEmitterTranslateDrag(rendern::Scene& scene)
    {
        scene.editorParticleEmitterTranslateDrag = {};
        scene.editorTranslateGizmo.activeAxis = rendern::GizmoAxis::None;
    }

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
        EndParticleEmitterTranslateDrag(scene);
    }

    inline void SyncCurrentGizmoVisual(
        EditorViewportInteraction& interaction,
        rendern::LevelAsset& levelAsset,
        rendern::LevelInstance& levelInstance,
        rendern::Scene& scene)
    {
        if (scene.editorGizmoMode == rendern::GizmoMode::Translate)
        {
            if (HasParticleEmitterSelection(scene))
            {
                SyncSelectedParticleEmitterTranslateGizmo(levelAsset, scene);
            }
            else
            {
                interaction.translateGizmo.SyncVisual(levelAsset, levelInstance, scene);
            }
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

        if (input.KeyPressed('X') && scene.editorGizmoMode == rendern::GizmoMode::Translate)
        {
            EndAllGizmoDrags(interaction, scene);
            scene.editorTranslateSpace =
                (scene.editorTranslateSpace == rendern::GizmoSpace::World)
                ? rendern::GizmoSpace::Local
                : rendern::GizmoSpace::World;
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
            if (HasParticleEmitterSelection(scene))
            {
                SyncSelectedParticleEmitterTranslateGizmo(levelAsset, scene);
            }
            else
            {
                interaction.translateGizmo.SyncVisual(levelAsset, levelInstance, scene);
            }
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
        int x = 0;
        int y = 0;
        if (!appWin32::TryGetCursorPosClient(hwnd, x, y))
        {
            return false;
        }

        if (x < 0 || y < 0 || x >= viewportWidth || y >= viewportHeight)
        {
            return false;
        }

        outMouseX = x;
        outMouseY = y;
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
            if (HasParticleEmitterSelection(scene))
            {
                scene.editorTranslateGizmo.hoveredAxis = rendern::HitTestTranslateGizmoAxis(scene, scene.editorTranslateGizmo, mouseXF, mouseYF, viewportWidthF, viewportHeightF);
                scene.editorRotateGizmo.hoveredAxis = rendern::GizmoAxis::None;
                scene.editorScaleGizmo.hoveredAxis = rendern::GizmoAxis::None;
            }
            else
            {
                interaction.translateGizmo.UpdateHover(scene, mouseXF, mouseYF, viewportWidthF, viewportHeightF);
                interaction.rotateGizmo.ClearHover(scene);
                interaction.scaleGizmo.ClearHover(scene);
            }
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

        int mouseX = 0;
        int mouseY = 0;
        if (!appWin32::TryGetCursorPosClient(hwnd, mouseX, mouseY))
        {
            return;
        }
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
            if (HasParticleEmitterSelection(scene))
            {
                if (scene.editorParticleEmitterTranslateDrag.dragging)
                {
                    if (input.KeyDown(VK_LBUTTON))
                    {
                        transformChanged = UpdateParticleEmitterTranslateDrag(levelAsset, levelInstance, scene, mouseXF, mouseYF, viewportWidthF, viewportHeightF);
                        gizmoConsumed = true;
                    }
                    else
                    {
                        EndParticleEmitterTranslateDrag(scene);
                        gizmoConsumed = true;
                    }
                }
                else if (input.KeyPressed(VK_LBUTTON))
                {
                    gizmoConsumed = TryBeginParticleEmitterTranslateDrag(levelAsset, scene, mouseXF, mouseYF, viewportWidthF, viewportHeightF);
                }
            }
            else if (interaction.translateGizmo.IsDragging())
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
            const bool ctrlDown = input.KeyDown(VK_CONTROL) || input.KeyDown(VK_LCONTROL) || input.KeyDown(VK_RCONTROL);

            const rendern::PickResult pick = rendern::PickEditorObjectUnderScreenPoint(
                scene,
                levelInstance,
                mouseXF,
                mouseYF,
                viewportWidthF,
                viewportHeightF);

            scene.debugPickRay.enabled = true;
            scene.debugPickRay.origin = pick.rayOrigin;
            scene.debugPickRay.direction = pick.rayDir;
            scene.debugPickRay.hit = ((pick.nodeIndex >= 0) || (pick.particleEmitterIndex >= 0)) && std::isfinite(pick.t);
            scene.debugPickRay.length = scene.debugPickRay.hit ? pick.t : scene.camera.farZ;

            if (scene.debugPickRay.hit && levelInstance.IsNodeAlive(levelAsset, pick.nodeIndex))
            {
                if (ctrlDown)
                {
                    scene.EditorToggleSelectionNode(pick.nodeIndex);
                }
                else
                {
                    scene.EditorSetSelectionSingle(pick.nodeIndex);
                }
            }
            else if (scene.debugPickRay.hit && levelInstance.IsValidParticleEmitterIndex(pick.particleEmitterIndex))
            {
                if (ctrlDown)
                {
                    scene.EditorToggleSelectionParticleEmitter(pick.particleEmitterIndex);
                }
                else
                {
                    scene.EditorSetSelectionSingleParticleEmitter(pick.particleEmitterIndex);
                }
            }
            else
            {
                // Click on empty space: clear selection only when Ctrl is not pressed.
                if (!ctrlDown)
                {
                    scene.EditorClearSelection();
                }
            }
        }
    }
} // namespace appEditor