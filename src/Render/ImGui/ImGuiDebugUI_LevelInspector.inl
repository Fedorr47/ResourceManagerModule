namespace rendern::ui::level_ui_detail
{
    static void DrawCreateImportSection(
        rendern::LevelAsset& level,
        rendern::LevelInstance& levelInst,
        AssetManager& assets,
        rendern::Scene& scene,
        rendern::CameraController& camCtl,
        LevelEditorUIState& st)
    {
        ImGui::Text("Create / Import");
        ImGui::Checkbox("Add as child of selected", &st.addAsChildOfSelection);

        const int parentForNew = ParentForNewNode(level, st);

        if (ImGui::Button("Add Cube"))
        {
            EnsureDefaultMesh(level, "cube", "models/cube.obj");
            const int newIdx = levelInst.AddNode(level, scene, assets, "cube", "", parentForNew, ComputeSpawnTransform(scene, camCtl), "Cube");
            st.selectedNode = newIdx;
        }
        ImGui::SameLine();
        if (ImGui::Button("Add Quad"))
        {
            EnsureDefaultMesh(level, "quad", "models/quad.obj");
            rendern::Transform t = ComputeSpawnTransform(scene, camCtl);
            t.scale = mathUtils::Vec3(3.0f, 1.0f, 3.0f);
            const int newIdx = levelInst.AddNode(level, scene, assets, "quad", "", parentForNew, t, "Quad");
            st.selectedNode = newIdx;
        }
        ImGui::SameLine();
        if (ImGui::Button("Add Planar Mirror"))
        {
            // Planar reflections are stencil-gated in DX12 renderer. This spawns a quad and assigns
            // a material with MaterialPerm::PlanarMirror.
            EnsureDefaultMesh(level, "quad", "models/quad.obj");

            const std::string matId = "planar_mirror";
            if (!level.materials.contains(matId))
            {
                rendern::LevelMaterialDef def{};
                def.material.permFlags = rendern::MaterialPerm::PlanarMirror; // not shadowed; overlay draws reflection
                def.material.params.baseColor = mathUtils::Vec4(1.0f, 1.0f, 1.0f, 1.0f);
                def.material.params.metallic = 1.0f;
                def.material.params.roughness = 0.02f;
                level.materials.emplace(matId, std::move(def));
            }

            // Make sure runtime material handle exists before spawning the node.
            levelInst.EnsureMaterial(level, scene, matId);

            rendern::Transform t = ComputeSpawnTransform(scene, camCtl);
            // Spawn as a vertical mirror facing the camera by default.
            t.rotationDegrees.x = 90.0f;
            t.scale = mathUtils::Vec3(3.0f, 1.0f, 3.0f);
            const int newIdx = levelInst.AddNode(level, scene, assets, "quad", matId, parentForNew, t, "PlanarMirror");
            st.selectedNode = newIdx;
        }
        ImGui::SameLine();
        if (ImGui::Button("Add Empty"))
        {
            const int newIdx = levelInst.AddNode(level, scene, assets, "", "", parentForNew, ComputeSpawnTransform(scene, camCtl), "Empty");
            st.selectedNode = newIdx;
        }

        ImGui::Spacing();
        ImGui::InputText("OBJ path", st.importPathBuf, sizeof(st.importPathBuf));

        if (ImGui::Button("Import mesh into library"))
        {
            const std::string pathStr = std::string(st.importPathBuf);
            if (!pathStr.empty())
            {
                std::string base = std::filesystem::path(pathStr).stem().string();
                if (base.empty())
                    base = "mesh";

                const std::string meshId = MakeUniqueMeshId(level, base);

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
            const std::string pathStr = std::string(st.importPathBuf);
            if (!pathStr.empty())
            {
                std::string base = std::filesystem::path(pathStr).stem().string();
                if (base.empty())
                    base = "mesh";

                const std::string meshId = MakeUniqueMeshId(level, base);

                if (!level.meshes.contains(meshId))
                {
                    rendern::LevelMeshDef def{};
                    def.path = pathStr;
                    def.debugName = meshId;
                    level.meshes.emplace(meshId, std::move(def));
                }

                const int newIdx = levelInst.AddNode(level, scene, assets, meshId, "", parentForNew, ComputeSpawnTransform(scene, camCtl), meshId);
                st.selectedNode = newIdx;
            }
        }
    }

    static void DrawSelectionInspector(
        rendern::LevelAsset& level,
        rendern::LevelInstance& levelInst,
        AssetManager& assets,
        rendern::Scene& scene,
        const DerivedLists& derived,
        LevelEditorUIState& st)
    {
        ImGui::Separator();
        ImGui::Text("Selection");

        if (st.selectedNode >= 0 && !NodeAlive(level, st.selectedNode))
            st.selectedNode = -1;

        if (st.selectedNode >= 0 && NodeAlive(level, st.selectedNode))
        {
            rendern::LevelNode& node = level.nodes[static_cast<std::size_t>(st.selectedNode)];

            if (st.prevSelectedNode != st.selectedNode)
            {
                std::snprintf(st.nameBuf, sizeof(st.nameBuf), "%s", node.name.c_str());
                st.prevSelectedNode = st.selectedNode;
            }

            ImGui::Text("Node #%d", st.selectedNode);

            if (ImGui::InputText("Name", st.nameBuf, sizeof(st.nameBuf)))
                node.name = std::string(st.nameBuf);

            bool vis = node.visible;
            if (ImGui::Checkbox("Visible", &vis))
                levelInst.SetNodeVisible(level, scene, assets, st.selectedNode, vis);

            // Mesh combo
            {
                std::vector<std::string> items;
                items.reserve(derived.meshIds.size() + 2);
                items.push_back("(none)");
                for (const auto& id : derived.meshIds) items.push_back(id);

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
                        levelInst.SetNodeMesh(level, scene, assets, st.selectedNode, "");
                    else
                        levelInst.SetNodeMesh(level, scene, assets, st.selectedNode, items[static_cast<std::size_t>(current)]);
                }
            }

            // Material combo
            {
                std::vector<std::string> items;
                items.reserve(derived.materialIds.size() + 2);
                items.push_back("(none)");
                for (const auto& id : derived.materialIds) items.push_back(id);

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
                        levelInst.SetNodeMaterial(level, scene, st.selectedNode, "");
                    else
                        levelInst.SetNodeMaterial(level, scene, st.selectedNode, items[static_cast<std::size_t>(current)]);
                }
            }

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
                levelInst.MarkTransformsDirty();

            ImGui::SeparatorText("Gizmo");
            int gizmoMode = static_cast<int>(scene.editorGizmoMode);
            constexpr const char* kGizmoModes[] = { "None", "Translate", "Rotate", "Scale" };
            if (ImGui::Combo("Mode", &gizmoMode, kGizmoModes, IM_ARRAYSIZE(kGizmoModes)))
                scene.editorGizmoMode = static_cast<rendern::GizmoMode>(gizmoMode);
            ImGui::TextUnformatted("Hotkeys: Q = None, W = Translate, E = Rotate, R = Scale, X = Toggle translate space");

            ImGui::SeparatorText("Translate Gizmo");
            bool gizmoEnabled = scene.editorTranslateGizmo.enabled;
            if (ImGui::Checkbox("Enable translate gizmo", &gizmoEnabled))
                scene.editorTranslateGizmo.enabled = gizmoEnabled;

            int translateSpace = static_cast<int>(scene.editorTranslateSpace);
            constexpr const char* kTranslateSpaceModes[] = { "World", "Local" };
            if (ImGui::Combo("Translate Space", &translateSpace, kTranslateSpaceModes, IM_ARRAYSIZE(kTranslateSpaceModes)))
                scene.editorTranslateSpace = static_cast<rendern::GizmoSpace>(translateSpace);

            ImGui::TextUnformatted("LMB drag axis X/Y/Z or plane handle XY/XZ/YZ in the main viewport. Hold Shift to snap by 0.5.");
            ImGui::Text("Translate space: %s", scene.editorTranslateSpace == rendern::GizmoSpace::World ? "World" : "Local");
            ImGui::Text("Visible: %s", scene.editorTranslateGizmo.visible ? "Yes" : "No");
            ImGui::Text("Hovered axis: %d", static_cast<int>(scene.editorTranslateGizmo.hoveredAxis));
            ImGui::Text("Active axis: %d", static_cast<int>(scene.editorTranslateGizmo.activeAxis));

            ImGui::SeparatorText("Rotate Gizmo");
            bool rotateGizmoEnabled = scene.editorRotateGizmo.enabled;
            if (ImGui::Checkbox("Enable rotate gizmo", &rotateGizmoEnabled))
                scene.editorRotateGizmo.enabled = rotateGizmoEnabled;

            ImGui::TextUnformatted("LMB drag local X/Y/Z rotation rings in the main viewport. Hold Shift to snap by 15 degrees.");
            ImGui::Text("Visible: %s", scene.editorRotateGizmo.visible ? "Yes" : "No");
            ImGui::Text("Hovered axis: %d", static_cast<int>(scene.editorRotateGizmo.hoveredAxis));
            ImGui::Text("Active axis: %d", static_cast<int>(scene.editorRotateGizmo.activeAxis));

            ImGui::SeparatorText("Scale Gizmo");
            bool scaleGizmoEnabled = scene.editorScaleGizmo.enabled;
            if (ImGui::Checkbox("Enable scale gizmo", &scaleGizmoEnabled))
                scene.editorScaleGizmo.enabled = scaleGizmoEnabled;

            ImGui::TextUnformatted("LMB drag local X/Y/Z scale handles, XY/XZ/YZ plane handles, or the center sphere for uniform scale in the main viewport. Hold Shift to snap by 0.1. Q/W/E/R switches modes.");
            ImGui::Text("Visible: %s", scene.editorScaleGizmo.visible ? "Yes" : "No");
            ImGui::Text("Hovered axis: %d", static_cast<int>(scene.editorScaleGizmo.hoveredAxis));
            ImGui::Text("Active axis: %d", static_cast<int>(scene.editorScaleGizmo.activeAxis));

            ImGui::Spacing();

            if (ImGui::Button("Duplicate"))
            {
                rendern::Transform t = node.transform;
                t.position.x += 1.0f;

                const int newIdx = levelInst.AddNode(level, scene, assets, node.mesh, node.material, node.parent, t, node.name);
                st.selectedNode = newIdx;
            }
            ImGui::SameLine();
            bool doDelete = ImGui::Button("Delete (recursive)");
            if (ImGui::IsKeyPressed(ImGuiKey_Delete))
                doDelete = true;

            if (doDelete)
            {
                const int parent = node.parent;
                levelInst.DeleteSubtree(level, scene, st.selectedNode);

                if (NodeAlive(level, parent))
                    st.selectedNode = parent;
                else
                    st.selectedNode = -1;
            }
        }
        else
        {
            ImGui::TextDisabled("No node selected.");
            st.prevSelectedNode = -2;
        }
    }

    static void DrawInspectorPanel(
        rendern::LevelAsset& level,
        rendern::LevelInstance& levelInst,
        AssetManager& assets,
        rendern::Scene& scene,
        rendern::CameraController& camCtl,
        const DerivedLists& derived,
        LevelEditorUIState& st)
    {
        ImGui::BeginChild("##Inspector", ImVec2(0.0f, 0.0f), true);

        DrawCreateImportSection(level, levelInst, assets, scene, camCtl, st);
        DrawSelectionInspector(level, levelInst, assets, scene, derived, st);

        ImGui::EndChild();
    }
}