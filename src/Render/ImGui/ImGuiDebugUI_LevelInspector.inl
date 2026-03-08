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
            st.selectedParticleEmitter = -1;
        }
        ImGui::SameLine();
        if (ImGui::Button("Add Quad"))
        {
            EnsureDefaultMesh(level, "quad", "models/quad.obj");
            rendern::Transform t = ComputeSpawnTransform(scene, camCtl);
            t.scale = mathUtils::Vec3(3.0f, 1.0f, 3.0f);
            const int newIdx = levelInst.AddNode(level, scene, assets, "quad", "", parentForNew, t, "Quad");
            st.selectedNode = newIdx;
            st.selectedParticleEmitter = -1;
        }
        ImGui::SameLine();
        if (ImGui::Button("Add Planar Mirror"))
        {
            EnsureDefaultMesh(level, "quad", "models/quad.obj");

            const std::string matId = "planar_mirror";
            if (!level.materials.contains(matId))
            {
                rendern::LevelMaterialDef def{};
                def.material.permFlags = rendern::MaterialPerm::PlanarMirror;
                def.material.params.baseColor = mathUtils::Vec4(1.0f, 1.0f, 1.0f, 1.0f);
                def.material.params.metallic = 1.0f;
                def.material.params.roughness = 0.02f;
                level.materials.emplace(matId, std::move(def));
            }

            levelInst.EnsureMaterial(level, scene, matId);

            rendern::Transform t = ComputeSpawnTransform(scene, camCtl);
            t.rotationDegrees.x = 90.0f;
            t.scale = mathUtils::Vec3(3.0f, 1.0f, 3.0f);
            const int newIdx = levelInst.AddNode(level, scene, assets, "quad", matId, parentForNew, t, "PlanarMirror");
            st.selectedNode = newIdx;
            st.selectedParticleEmitter = -1;
        }
        ImGui::SameLine();
        if (ImGui::Button("Add Empty"))
        {
            const int newIdx = levelInst.AddNode(level, scene, assets, "", "", parentForNew, ComputeSpawnTransform(scene, camCtl), "Empty");
            st.selectedNode = newIdx;
            st.selectedParticleEmitter = -1;
        }

        if (ImGui::Button("Add Particle Emitter"))
        {
            rendern::ParticleEmitter emitter{};
            emitter.name = "ParticleEmitter";
            emitter.position = ComputeSpawnTransform(scene, camCtl).position;
            const int newIdx = levelInst.AddParticleEmitter(level, scene, emitter);
            st.selectedNode = -1;
            st.selectedParticleEmitter = newIdx;
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

                try
                {
                    rendern::MeshProperties p{};
                    p.filePath = pathStr;
                    p.debugName = meshId;
                    assets.LoadMeshAsync(meshId, std::move(p));
                }
                catch (...)
                {
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
                st.selectedParticleEmitter = -1;
            }
        }
    }

    static void DrawNodeSelectionInspector(
        rendern::LevelAsset& level,
        rendern::LevelInstance& levelInst,
        AssetManager& assets,
        rendern::Scene& scene,
        const DerivedLists& derived,
        LevelEditorUIState& st)
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
            st.selectedParticleEmitter = -1;
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
            st.selectedParticleEmitter = -1;
        }
    }

    static void DrawParticleEmitterSelectionInspector(
        rendern::LevelAsset& level,
        rendern::LevelInstance& levelInst,
        rendern::Scene& scene,
        LevelEditorUIState& st)
    {
        rendern::ParticleEmitter& emitter = level.particleEmitters[static_cast<std::size_t>(st.selectedParticleEmitter)];

        if (st.prevSelectedParticleEmitter != st.selectedParticleEmitter)
        {
            std::snprintf(st.nameBuf, sizeof(st.nameBuf), "%s", emitter.name.c_str());
            st.prevSelectedParticleEmitter = st.selectedParticleEmitter;
        }

        ImGui::Text("Particle Emitter #%d", st.selectedParticleEmitter);

        bool changed = false;

        if (ImGui::InputText("Name", st.nameBuf, sizeof(st.nameBuf)))
        {
            emitter.name = std::string(st.nameBuf);
            changed = true;
        }

        changed |= ImGui::Checkbox("Enabled", &emitter.enabled);
        changed |= ImGui::Checkbox("Looping", &emitter.looping);
        changed |= DragVec3("Position", emitter.position, 0.05f);
        changed |= DragVec3("Position Jitter", emitter.positionJitter, 0.02f);
        changed |= DragVec3("Velocity Min", emitter.velocityMin, 0.02f);
        changed |= DragVec3("Velocity Max", emitter.velocityMax, 0.02f);

        float color[4] = { emitter.color.x, emitter.color.y, emitter.color.z, emitter.color.w };
        if (ImGui::ColorEdit4("Color", color))
        {
            emitter.color = mathUtils::Vec4(color[0], color[1], color[2], color[3]);
            changed = true;
        }

        float sizeMin = emitter.sizeMin;
        float sizeMax = emitter.sizeMax;
        if (ImGui::DragFloat("Size Min", &sizeMin, 0.01f, 0.001f, 100.0f, "%.3f"))
        {
            emitter.sizeMin = std::max(0.001f, sizeMin);
            if (emitter.sizeMax < emitter.sizeMin)
                emitter.sizeMax = emitter.sizeMin;
            changed = true;
        }
        if (ImGui::DragFloat("Size Max", &sizeMax, 0.01f, 0.001f, 100.0f, "%.3f"))
        {
            emitter.sizeMax = std::max(emitter.sizeMin, sizeMax);
            changed = true;
        }

        float lifetimeMin = emitter.lifetimeMin;
        float lifetimeMax = emitter.lifetimeMax;
        if (ImGui::DragFloat("Lifetime Min", &lifetimeMin, 0.01f, 0.001f, 100.0f, "%.3f"))
        {
            emitter.lifetimeMin = std::max(0.001f, lifetimeMin);
            if (emitter.lifetimeMax < emitter.lifetimeMin)
                emitter.lifetimeMax = emitter.lifetimeMin;
            changed = true;
        }
        if (ImGui::DragFloat("Lifetime Max", &lifetimeMax, 0.01f, 0.001f, 100.0f, "%.3f"))
        {
            emitter.lifetimeMax = std::max(emitter.lifetimeMin, lifetimeMax);
            changed = true;
        }

        changed |= ImGui::DragFloat("Spawn Rate", &emitter.spawnRate, 0.1f, 0.0f, 100000.0f, "%.3f");

        int burstCount = static_cast<int>(emitter.burstCount);
        if (ImGui::DragInt("Burst Count", &burstCount, 1.0f, 0, 100000))
        {
            emitter.burstCount = static_cast<std::uint32_t>(std::max(0, burstCount));
            changed = true;
        }

        int maxParticles = static_cast<int>(emitter.maxParticles);
        if (ImGui::DragInt("Max Particles", &maxParticles, 1.0f, 0, 100000))
        {
            emitter.maxParticles = static_cast<std::uint32_t>(std::max(0, maxParticles));
            changed = true;
        }

        changed |= ImGui::DragFloat("Duration", &emitter.duration, 0.05f, 0.0f, 100000.0f, "%.3f");
        changed |= ImGui::DragFloat("Start Delay", &emitter.startDelay, 0.05f, 0.0f, 100000.0f, "%.3f");

        if (changed)
        {
            levelInst.RestartParticleEmitter(level, scene, st.selectedParticleEmitter);
        }

        ImGui::SeparatorText("Runtime");
        if (const rendern::ParticleEmitter* runtimeEmitter = levelInst.GetRuntimeParticleEmitter(static_cast<const rendern::Scene&>(scene), st.selectedParticleEmitter))
        {
            int aliveCount = 0;
            for (const rendern::Particle& particle : scene.particles)
            {
                if (particle.alive && particle.ownerEmitter == st.selectedParticleEmitter)
                {
                    ++aliveCount;
                }
            }

            ImGui::Text("Alive particles: %d", aliveCount);
            ImGui::Text("World position: %.2f %.2f %.2f", runtimeEmitter->position.x, runtimeEmitter->position.y, runtimeEmitter->position.z);
            ImGui::Text("Elapsed: %.3f", runtimeEmitter->elapsed);
            ImGui::Text("Spawn accumulator: %.3f", runtimeEmitter->spawnAccumulator);
            ImGui::Text("Burst done: %s", runtimeEmitter->burstDone ? "Yes" : "No");
        }
        else
        {
            ImGui::TextDisabled("Runtime emitter is not instantiated.");
        }

        if (ImGui::Button("Restart Emitter"))
        {
            levelInst.RestartParticleEmitter(level, scene, st.selectedParticleEmitter);
        }
        ImGui::SameLine();
        if (ImGui::Button("Burst Now"))
        {
            levelInst.TriggerParticleEmitterBurst(level, scene, st.selectedParticleEmitter);
        }

        if (ImGui::Button("Duplicate Emitter"))
        {
            rendern::ParticleEmitter copy = emitter;
            copy.name += "_Copy";
            const int newIdx = levelInst.AddParticleEmitter(level, scene, copy);
            scene.EditorSetSelectionSingleParticleEmitter(newIdx);
            levelInst.SyncEditorRuntimeBindings(level, scene);
            st.selectedParticleEmitter = newIdx;
            st.prevSelectedParticleEmitter = -2;
            return;
        }
        ImGui::SameLine();
        if (ImGui::Button("Move To Camera"))
        {
            mathUtils::Vec3 forward = scene.camera.target - scene.camera.position;
            if (mathUtils::Length(forward) <= 1e-5f)
            {
                forward = mathUtils::Vec3(0.0f, 0.0f, -1.0f);
            }
            else
            {
                forward = mathUtils::Normalize(forward);
            }
            emitter.position = scene.camera.position + forward * 3.0f;
            levelInst.SetParticleEmitterPosition(level, scene, st.selectedParticleEmitter, emitter.position);
            levelInst.RestartParticleEmitter(level, scene, st.selectedParticleEmitter);
            changed = false;
        }
        ImGui::SameLine();
        bool doDeleteEmitter = ImGui::Button("Delete Emitter");
        if (ImGui::IsKeyPressed(ImGuiKey_Delete))
        {
            doDeleteEmitter = true;
        }
        if (doDeleteEmitter)
        {
            levelInst.DeleteParticleEmitter(level, scene, st.selectedParticleEmitter);
            st.selectedParticleEmitter = -1;
            st.prevSelectedParticleEmitter = -2;
            scene.editorSelectedParticleEmitter = -1;
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

        if (st.selectedParticleEmitter >= 0 && !ParticleEmitterAlive(level, st.selectedParticleEmitter))
            st.selectedParticleEmitter = -1;
        if (st.selectedNode >= 0 && !NodeAlive(level, st.selectedNode))
            st.selectedNode = -1;

        if (st.selectedParticleEmitter >= 0)
        {
            st.selectedNode = -1;
            DrawParticleEmitterSelectionInspector(level, levelInst, scene, st);
            st.prevSelectedNode = -2;
            return;
        }

        {
            const int selectedCount = static_cast<int>(scene.editorSelectedNodes.size());
            if (selectedCount > 1)
            {
                ImGui::Text("Multi-selection: %d nodes", selectedCount);
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear"))
                {
                    scene.EditorClearSelection();
                    st.selectedNode = -1;
                }
                ImGui::Text("Primary: #%d", scene.editorSelectedNode);
                st.selectedNode = scene.editorSelectedNode;
                ImGui::Separator();
            }
        }

        if (st.selectedNode >= 0 && NodeAlive(level, st.selectedNode))
        {
            DrawNodeSelectionInspector(level, levelInst, assets, scene, derived, st);
        }
        else
        {
            ImGui::TextDisabled("No node or emitter selected.");
            st.prevSelectedNode = -2;
            st.prevSelectedParticleEmitter = -2;
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
