namespace rendern::ui::level_ui_detail
{
    static constexpr const char* kDemoSmokeTextureId = "particle_smoke_soft_01";
    static constexpr const char* kDemoSmokeTexturePath = "textures/particles/soft_smoke_01.png";

    static std::string MakeUniqueParticleEmitterName(const rendern::LevelAsset& level, std::string_view base)
    {
        auto NameExists = [&](std::string_view candidate) noexcept
            {
                for (const rendern::ParticleEmitter& emitter : level.particleEmitters)
                {
                    if (emitter.name == candidate)
                    {
                        return true;
                    }
                }
                return false;
            };

        std::string result = std::string(base.empty() ? std::string_view("ParticleEmitter") : base);
        if (!NameExists(result))
        {
            return result;
        }

        for (int suffix = 2; suffix <= 9999; ++suffix)
        {
            std::string candidate = result + std::to_string(suffix);
            if (!NameExists(candidate))
            {
                return candidate;
            }
        }

        return result + "_Copy";
    }

    static void EnsureDemoSmokeTexture(rendern::LevelAsset& level, AssetManager& assets)
    {
        auto it = level.textures.find(kDemoSmokeTextureId);
        if (it == level.textures.end())
        {
            rendern::LevelTextureDef def{};
            def.kind = rendern::LevelTextureKind::Tex2D;
            def.props.dimension = TextureDimension::Tex2D;
            def.props.filePath = kDemoSmokeTexturePath;
            def.props.srgb = true;
            def.props.generateMips = true;
            def.props.flipY = false;
            it = level.textures.emplace(kDemoSmokeTextureId, std::move(def)).first;
        }

        TextureProperties props = it->second.props;
        if (props.filePath.empty())
        {
            props.dimension = TextureDimension::Tex2D;
            props.filePath = kDemoSmokeTexturePath;
            props.srgb = true;
            props.generateMips = true;
            props.flipY = false;
            it->second.props = props;
        }

        assets.LoadTextureAsync(kDemoSmokeTextureId, std::move(props));
    }

    static rendern::ParticleEmitter MakeDemoSmokeEmitter(const rendern::LevelAsset& level, const rendern::Scene& scene, const rendern::CameraController& camCtl)
    {
        rendern::ParticleEmitter emitter{};
        emitter.name = MakeUniqueParticleEmitterName(level, "SmokeSoft");
        emitter.textureId = kDemoSmokeTextureId;
        emitter.position = ComputeSpawnTransform(scene, camCtl).position;
        emitter.positionJitter = mathUtils::Vec3(0.18f, 0.04f, 0.18f);
        emitter.velocityMin = mathUtils::Vec3(-0.08f, 0.28f, -0.08f);
        emitter.velocityMax = mathUtils::Vec3(0.08f, 0.62f, 0.08f);
        emitter.colorBegin = mathUtils::Vec4(0.95f, 0.95f, 0.95f, 0.55f);
        emitter.colorEnd = mathUtils::Vec4(0.55f, 0.58f, 0.60f, 0.0f);
        emitter.sizeMin = 0.28f;
        emitter.sizeMax = 0.42f;
        emitter.sizeBegin = 0.24f;
        emitter.sizeEnd = 0.95f;
        emitter.lifetimeMin = 1.2f;
        emitter.lifetimeMax = 2.1f;
        emitter.spawnRate = 18.0f;
        emitter.burstCount = 8u;
        emitter.maxParticles = 256u;
        emitter.looping = true;
        emitter.duration = 0.0f;
        emitter.startDelay = 0.0f;
        return emitter;
    }

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
            emitter.name = MakeUniqueParticleEmitterName(level, "ParticleEmitter");
            emitter.position = ComputeSpawnTransform(scene, camCtl).position;
            const int newIdx = levelInst.AddParticleEmitter(level, scene, emitter);
            st.selectedNode = -1;
            st.selectedParticleEmitter = newIdx;
        }
        ImGui::SameLine();
        if (ImGui::Button("Add Demo Smoke Emitter"))
        {
            EnsureDemoSmokeTexture(level, assets);
            const int newIdx = levelInst.AddParticleEmitter(level, scene, MakeDemoSmokeEmitter(level, scene, camCtl));
            st.selectedNode = -1;
            st.selectedParticleEmitter = newIdx;
        }
        ImGui::TextDisabled("Demo texture: %s", kDemoSmokeTexturePath);

        ImGui::Spacing();
        ImGui::InputText("OBJ path", st.importPathBuf, sizeof(st.importPathBuf));
        ImGui::InputText("Asset id (optional)", st.importAssetIdBuf, sizeof(st.importAssetIdBuf));
        ImGui::Checkbox("Flip UVs on import", &st.importFlipUVs);

        const auto makeImportBaseId = [&](const char* fallback) -> std::string
        {
            std::string base = std::string(st.importAssetIdBuf);
            if (base.empty())
            {
                base = std::filesystem::path(std::string(st.importPathBuf)).stem().string();
            }
            if (base.empty())
            {
                base = fallback;
            }
            return base;
        };

        if (ImGui::Button("Import mesh into library"))
        {
            const std::string pathStr = std::string(st.importPathBuf);
            if (!pathStr.empty())
            {
                const std::string meshId = MakeUniqueMeshId(level, makeImportBaseId("mesh"));

                rendern::LevelMeshDef def{};
                def.path = pathStr;
                def.debugName = meshId;
                def.flipUVs = st.importFlipUVs;
                level.meshes.emplace(meshId, std::move(def));

                try
                {
                    rendern::MeshProperties p{};
                    p.filePath = pathStr;
                    p.debugName = meshId;
                    p.flipUVs = st.importFlipUVs;
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
                const std::string base = makeImportBaseId("mesh");
                const std::string meshId = level.meshes.contains(base) ? base : MakeUniqueMeshId(level, base);

                if (!level.meshes.contains(meshId))
                {
                    rendern::LevelMeshDef def{};
                    def.path = pathStr;
                    def.debugName = meshId;
                    def.flipUVs = st.importFlipUVs;
                    level.meshes.emplace(meshId, std::move(def));
                }

                const int newIdx = levelInst.AddNode(level, scene, assets, meshId, "", parentForNew, ComputeSpawnTransform(scene, camCtl), meshId);
                st.selectedNode = newIdx;
                st.selectedParticleEmitter = -1;
            }
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Skinned Import");
        if (ImGui::Button("Import skinned mesh into library"))
        {
            const std::string pathStr = std::string(st.importPathBuf);
            if (!pathStr.empty())
            {
                const std::string skinnedId = MakeUniqueSkinnedMeshId(level, makeImportBaseId("skinned"));
                rendern::LevelSkinnedMeshDef def{};
                def.path = pathStr;
                def.debugName = skinnedId;
                def.flipUVs = st.importFlipUVs;
                level.skinnedMeshes.emplace(skinnedId, std::move(def));
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Create skinned object"))
        {
            const std::string pathStr = std::string(st.importPathBuf);
            if (!pathStr.empty())
            {
                const std::string base = makeImportBaseId("skinned");
                const std::string skinnedId = level.skinnedMeshes.contains(base) ? base : MakeUniqueSkinnedMeshId(level, base);
                if (!level.skinnedMeshes.contains(skinnedId))
                {
                    rendern::LevelSkinnedMeshDef def{};
                    def.path = pathStr;
                    def.debugName = skinnedId;
                    def.flipUVs = st.importFlipUVs;
                    level.skinnedMeshes.emplace(skinnedId, std::move(def));
                }

                const int newIdx = levelInst.AddNode(level, scene, assets, "", "", parentForNew, ComputeSpawnTransform(scene, camCtl), skinnedId);
                levelInst.SetNodeSkinnedMesh(level, scene, assets, newIdx, skinnedId);
                st.selectedNode = newIdx;
                st.selectedParticleEmitter = -1;
            }
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Model / Scene Import");
        ImGui::Checkbox("Import skeleton/bone nodes (scene debug only)", &st.importSceneSkeletonNodes);

        if (ImGui::Button("Import model into library"))
        {
            const std::string pathStr = std::string(st.importPathBuf);
            if (!pathStr.empty())
            {
                const std::string modelId = MakeUniqueModelId(level, makeImportBaseId("model"));
                rendern::LevelModelDef def{};
                def.path = pathStr;
                def.debugName = modelId;
                def.flipUVs = st.importFlipUVs;
                level.models.emplace(modelId, std::move(def));
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Create model object"))
        {
            const std::string pathStr = std::string(st.importPathBuf);
            if (!pathStr.empty())
            {
                const std::string base = makeImportBaseId("model");
                const std::string modelId = level.models.contains(base) ? base : MakeUniqueModelId(level, base);
                if (!level.models.contains(modelId))
                {
                    rendern::LevelModelDef def{};
                    def.path = pathStr;
                    def.debugName = modelId;
                    def.flipUVs = st.importFlipUVs;
                    level.models.emplace(modelId, std::move(def));
                }

                const int newIdx = levelInst.AddNode(level, scene, assets, "", "", parentForNew, ComputeSpawnTransform(scene, camCtl), modelId);
                levelInst.SetNodeModel(level, scene, assets, newIdx, modelId);
                st.selectedNode = newIdx;
                st.selectedParticleEmitter = -1;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Import model scene as nodes"))
        {
            const std::string pathStr = std::string(st.importPathBuf);
            if (!pathStr.empty())
            {
                const std::string base = makeImportBaseId("model");
                const std::string modelId = level.models.contains(base) ? base : MakeUniqueModelId(level, base);
                if (!level.models.contains(modelId))
                {
                    rendern::LevelModelDef def{};
                    def.path = pathStr;
                    def.debugName = modelId;
                    def.flipUVs = st.importFlipUVs;
                    level.models.emplace(modelId, std::move(def));
                }

                try
                {
                    const int firstIdx = levelInst.ImportModelSceneAsNodes(
                        level,
                        scene,
                        assets,
                        modelId,
                        parentForNew,
                        st.importSceneCreateMaterialPlaceholders,
                        st.importSceneSkeletonNodes,
                        true);
                    st.selectedNode = firstIdx;
                    st.selectedParticleEmitter = -1;
                }
                catch (...)
                {
                }
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
            items.reserve(derived.modelIds.size() + 2);
            items.push_back("(none)");
            for (const auto& id : derived.modelIds) items.push_back(id);

            if (!node.model.empty() && !level.models.contains(node.model))
                items.push_back(std::string("<missing> ") + node.model);

            int current = 0;
            if (!node.model.empty())
            {
                for (std::size_t i = 1; i < items.size(); ++i)
                {
                    if (items[i] == node.model)
                    {
                        current = static_cast<int>(i);
                        break;
                    }
                }
            }

            std::vector<const char*> citems;
            citems.reserve(items.size());
            for (auto& s : items) citems.push_back(s.c_str());

            if (ImGui::Combo("Model", &current, citems.data(), static_cast<int>(citems.size())))
            {
                if (current == 0)
                    levelInst.SetNodeModel(level, scene, assets, st.selectedNode, "");
                else
                    levelInst.SetNodeModel(level, scene, assets, st.selectedNode, items[static_cast<std::size_t>(current)]);
            }
        }

        {
            std::vector<std::string> items;
            items.reserve(derived.skinnedMeshIds.size() + 2);
            items.push_back("(none)");
            for (const auto& id : derived.skinnedMeshIds) items.push_back(id);

            if (!node.skinnedMesh.empty() && !level.skinnedMeshes.contains(node.skinnedMesh))
                items.push_back(std::string("<missing> ") + node.skinnedMesh);

            int current = 0;
            if (!node.skinnedMesh.empty())
            {
                for (std::size_t i = 1; i < items.size(); ++i)
                {
                    if (items[i] == node.skinnedMesh)
                    {
                        current = static_cast<int>(i);
                        break;
                    }
                }
            }

            std::vector<const char*> citems;
            citems.reserve(items.size());
            for (auto& s : items) citems.push_back(s.c_str());

            if (ImGui::Combo("Skinned Mesh", &current, citems.data(), static_cast<int>(citems.size())))
            {
                if (current == 0)
                    levelInst.SetNodeSkinnedMesh(level, scene, assets, st.selectedNode, "");
                else
                    levelInst.SetNodeSkinnedMesh(level, scene, assets, st.selectedNode, items[static_cast<std::size_t>(current)]);
            }
        }

        {
            const bool isModelNode = !node.model.empty();
            const bool isSkinnedNode = !node.skinnedMesh.empty();
            ImGui::TextDisabled("Node kind: %s", isSkinnedNode ? "Skinned" : (isModelNode ? "Model" : (!node.mesh.empty() ? "Mesh" : "Empty")));
            if (isModelNode)
            {
                auto itModel = level.models.find(node.model);
                if (itModel != level.models.end())
                {
                    ImGui::TextDisabled("Model path: %s", itModel->second.path.c_str());
                    try
                    {
                        const rendern::ImportedModelScene meta = rendern::LoadAssimpScene(itModel->second.path, itModel->second.flipUVs);
                        ImGui::TextDisabled("Submeshes: %d", static_cast<int>(meta.submeshes.size()));
                        ImGui::SeparatorText("Material Overrides");
                        for (const rendern::ImportedSubmeshInfo& sub : meta.submeshes)
                        {
                            std::vector<std::string> overrideItems;
                            overrideItems.reserve(derived.materialIds.size() + 2);
                            overrideItems.push_back("(default)");
                            for (const auto& id : derived.materialIds) overrideItems.push_back(id);

                            int overrideCurrent = 0;
                            if (auto itOv = node.materialOverrides.find(sub.submeshIndex); itOv != node.materialOverrides.end())
                            {
                                for (std::size_t oi = 1; oi < overrideItems.size(); ++oi)
                                {
                                    if (overrideItems[oi] == itOv->second)
                                    {
                                        overrideCurrent = static_cast<int>(oi);
                                        break;
                                    }
                                }
                            }

                            std::vector<const char*> overrideCItems;
                            overrideCItems.reserve(overrideItems.size());
                            for (auto& s : overrideItems) overrideCItems.push_back(s.c_str());

                            const std::string label = "Submesh " + std::to_string(sub.submeshIndex) + "##mat_override" + std::to_string(sub.submeshIndex);
                            if (ImGui::Combo(label.c_str(), &overrideCurrent, overrideCItems.data(), static_cast<int>(overrideCItems.size())))
                            {
                                if (overrideCurrent == 0)
                                    levelInst.SetNodeMaterialOverride(level, scene, assets, st.selectedNode, sub.submeshIndex, "");
                                else
                                    levelInst.SetNodeMaterialOverride(level, scene, assets, st.selectedNode, sub.submeshIndex, overrideItems[static_cast<std::size_t>(overrideCurrent)]);
                            }
                            ImGui::SameLine();
                            ImGui::TextDisabled("%s", sub.name.c_str());
                        }
                    }
                    catch (...)
                    {
                        ImGui::TextDisabled("Failed to read model metadata.");
                    }
                }
            }
        }

        {
            const bool isSkinnedNode = !node.skinnedMesh.empty();
            const int skinnedDrawIndex = levelInst.GetNodeSkinnedDrawIndex(st.selectedNode);
            rendern::SkinnedDrawItem* skinnedItem = levelInst.GetSkinnedDrawItem(scene, skinnedDrawIndex);
            if (isSkinnedNode)
            {
                auto itSkinned = level.skinnedMeshes.find(node.skinnedMesh);
                if (itSkinned != level.skinnedMeshes.end())
                {
                    ImGui::TextDisabled("Skinned path: %s", itSkinned->second.path.c_str());
                }

                ImGui::SeparatorText("Animation");
                if (skinnedItem && skinnedItem->asset)
                {
                    const auto& skeleton = skinnedItem->asset->mesh.skeleton;
                    const std::size_t boneCount = skeleton.bones.size();
                    const std::size_t clipCount = skinnedItem->asset->clips.size();
                    const std::string activeClipName =
                        (skinnedItem->activeClipIndex >= 0 && static_cast<std::size_t>(skinnedItem->activeClipIndex) < skinnedItem->asset->clips.size())
                        ? skinnedItem->asset->clips[static_cast<std::size_t>(skinnedItem->activeClipIndex)].name
                        : std::string("<none>");

                    ImGui::Text("Bones: %d", static_cast<int>(boneCount));
                    ImGui::Text("Clips: %d", static_cast<int>(clipCount));
                    ImGui::Text("Active clip: %s", activeClipName.c_str());
                    ImGui::Text("Palette size: %d", static_cast<int>(skinnedItem->animator.skinMatrices.size()));

                    std::vector<std::string> clipItems;
                    clipItems.reserve(skinnedItem->asset->clips.size() + 1);
                    clipItems.push_back("(bind pose)");
                    for (const auto& clip : skinnedItem->asset->clips)
                    {
                        clipItems.push_back(clip.name.empty() ? std::string("<unnamed clip>") : clip.name);
                    }

                    int clipCurrent = 0;
                    if (skinnedItem->activeClipIndex >= 0)
                    {
                        clipCurrent = skinnedItem->activeClipIndex + 1;
                    }
                    std::vector<const char*> clipCItems;
                    clipCItems.reserve(clipItems.size());
                    for (auto& s : clipItems) clipCItems.push_back(s.c_str());
                    if (ImGui::Combo("Clip", &clipCurrent, clipCItems.data(), static_cast<int>(clipCItems.size())))
                    {
                        if (clipCurrent <= 0)
                        {
                            node.animationClip.clear();
                            skinnedItem->activeClipIndex = -1;
                            skinnedItem->debugForceBindPose = true;
                            skinnedItem->autoplay = false;
                            skinnedItem->animator.paused = true;
                            ResetAnimatorToBindPose(skinnedItem->animator, skinnedItem->asset->mesh.skeleton);
                            EvaluateAnimator(skinnedItem->animator);
                        }
                        else
                        {
                            const int newClipIndex = clipCurrent - 1;
                            node.animationClip = skinnedItem->asset->clips[static_cast<std::size_t>(newClipIndex)].name;
                            skinnedItem->activeClipIndex = newClipIndex;
                            skinnedItem->debugForceBindPose = false;
                            SetAnimatorClip(
                                skinnedItem->animator,
                                skinnedItem->asset->mesh.skeleton,
                                skinnedItem->asset->clips,
                                newClipIndex,
                                node.animationLoop,
                                node.animationPlayRate,
                                true);
                            skinnedItem->autoplay = node.animationAutoplay;
                            skinnedItem->animator.paused = !node.animationAutoplay;
                            EvaluateAnimator(skinnedItem->animator);
                        }
                    }

                    bool autoplay = node.animationAutoplay;
                    if (ImGui::Checkbox("Autoplay", &autoplay))
                    {
                        node.animationAutoplay = autoplay;
                        skinnedItem->autoplay = autoplay;
                        if (autoplay)
                        {
                            skinnedItem->debugForceBindPose = false;
                            skinnedItem->animator.paused = false;
                        }
                        else
                        {
                            skinnedItem->animator.paused = true;
                        }
                        EvaluateAnimator(skinnedItem->animator);
                    }

                    bool loop = node.animationLoop;
                    if (ImGui::Checkbox("Loop", &loop))
                    {
                        node.animationLoop = loop;
                        skinnedItem->animator.looping = loop;
                        if (skinnedItem->animator.clip)
                        {
                            skinnedItem->animator.timeSeconds = NormalizeAnimationTimeSeconds(*skinnedItem->animator.clip, skinnedItem->animator.timeSeconds, loop);
                        }
                        EvaluateAnimator(skinnedItem->animator);
                    }

                    float playRate = node.animationPlayRate;
                    if (ImGui::SliderFloat("Play rate", &playRate, 0.0f, 4.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp))
                    {
                        node.animationPlayRate = playRate;
                        skinnedItem->animator.playRate = playRate;
                    }

                    bool bindPose = skinnedItem->debugForceBindPose;
                    if (ImGui::Checkbox("Bind pose preview", &bindPose))
                    {
                        skinnedItem->debugForceBindPose = bindPose;
                        if (bindPose)
                        {
                            skinnedItem->autoplay = false;
                            skinnedItem->animator.paused = true;
                            ResetAnimatorToBindPose(skinnedItem->animator, skinnedItem->asset->mesh.skeleton);
                        }
                        EvaluateAnimator(skinnedItem->animator);
                    }

                    if (ImGui::Button(skinnedItem->autoplay && !skinnedItem->animator.paused ? "Pause" : "Play"))
                    {
                        const bool willPlay = skinnedItem->animator.paused || !skinnedItem->autoplay;
                        node.animationAutoplay = willPlay;
                        skinnedItem->autoplay = willPlay;
                        skinnedItem->animator.paused = !willPlay;
                        if (willPlay)
                        {
                            skinnedItem->debugForceBindPose = false;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Restart clip"))
                    {
                        skinnedItem->animator.timeSeconds = 0.0f;
                        skinnedItem->debugForceBindPose = false;
                        EvaluateAnimator(skinnedItem->animator);
                    }

                    if (skinnedItem->animator.clip != nullptr)
                    {
                        const float clipDurationSeconds = (skinnedItem->animator.clip->ticksPerSecond > 0.0f)
                            ? (skinnedItem->animator.clip->durationTicks / skinnedItem->animator.clip->ticksPerSecond)
                            : 0.0f;
                        float scrubTime = NormalizeAnimationTimeSeconds(*skinnedItem->animator.clip, skinnedItem->animator.timeSeconds, skinnedItem->animator.looping);
                        if (ImGui::SliderFloat("Time", &scrubTime, 0.0f, std::max(0.0f, clipDurationSeconds), "%.3f", ImGuiSliderFlags_AlwaysClamp))
                        {
                            skinnedItem->animator.timeSeconds = scrubTime;
                            skinnedItem->animator.paused = true;
                            skinnedItem->autoplay = false;
                            node.animationAutoplay = false;
                            skinnedItem->debugForceBindPose = false;
                            EvaluateAnimator(skinnedItem->animator);
                        }
                        ImGui::TextDisabled("Duration: %.3f s", clipDurationSeconds);
                    }
                    else
                    {
                        ImGui::TextDisabled("No active clip. Skeleton stays in bind pose.");
                    }

                    ImGui::SeparatorText("Skinned Debug");
                    ImGui::Checkbox("Draw selected skeleton", &scene.editorDrawSelectedSkinnedSkeleton);
                    ImGui::Checkbox("Draw selected max bounds", &scene.editorDrawSelectedSkinnedBounds);
                }
                else
                {
                    ImGui::TextDisabled("Runtime skinned draw is not instantiated.");
                }
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
            if (!node.model.empty())
            {
                levelInst.SetNodeModel(level, scene, assets, newIdx, node.model);
                for (const auto& [submeshIndex, materialId] : node.materialOverrides)
                {
                    levelInst.SetNodeMaterialOverride(level, scene, assets, newIdx, submeshIndex, materialId);
                }
            }
            if (!node.skinnedMesh.empty())
            {
                levelInst.SetNodeSkinnedMesh(level, scene, assets, newIdx, node.skinnedMesh);
                rendern::LevelNode& dup = level.nodes[static_cast<std::size_t>(newIdx)];
                dup.animationClip = node.animationClip;
                dup.animationAutoplay = node.animationAutoplay;
                dup.animationLoop = node.animationLoop;
                dup.animationPlayRate = node.animationPlayRate;
                levelInst.SetNodeSkinnedMesh(level, scene, assets, newIdx, node.skinnedMesh);
            }
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

        char textureIdBuf[256]{};
        std::snprintf(textureIdBuf, sizeof(textureIdBuf), "%s", emitter.textureId.c_str());
        if (ImGui::InputText("Texture Id", textureIdBuf, sizeof(textureIdBuf)))
        {
            emitter.textureId = std::string(textureIdBuf);
            changed = true;
        }

        changed |= ImGui::Checkbox("Enabled", &emitter.enabled);
        changed |= ImGui::Checkbox("Looping", &emitter.looping);
        changed |= DragVec3("Position", emitter.position, 0.05f);
        changed |= DragVec3("Position Jitter", emitter.positionJitter, 0.02f);
        changed |= DragVec3("Velocity Min", emitter.velocityMin, 0.02f);
        changed |= DragVec3("Velocity Max", emitter.velocityMax, 0.02f);

        float colorBegin[4] = { emitter.colorBegin.x, emitter.colorBegin.y, emitter.colorBegin.z, emitter.colorBegin.w };
        if (ImGui::ColorEdit4("Color Begin", colorBegin))
        {
            emitter.colorBegin = mathUtils::Vec4(colorBegin[0], colorBegin[1], colorBegin[2], colorBegin[3]);
            changed = true;
        }

        float colorEnd[4] = { emitter.colorEnd.x, emitter.colorEnd.y, emitter.colorEnd.z, emitter.colorEnd.w };
        if (ImGui::ColorEdit4("Color End", colorEnd))
        {
            emitter.colorEnd = mathUtils::Vec4(colorEnd[0], colorEnd[1], colorEnd[2], colorEnd[3]);
            changed = true;
        }

        float sizeBegin = emitter.sizeBegin;
        float sizeEnd = emitter.sizeEnd;
        if (ImGui::DragFloat("Size Begin", &sizeBegin, 0.01f, 0.001f, 100.0f, "%.3f"))
        {
            emitter.sizeBegin = std::max(0.001f, sizeBegin);
            changed = true;
        }
        if (ImGui::DragFloat("Size End", &sizeEnd, 0.01f, 0.001f, 100.0f, "%.3f"))
        {
            emitter.sizeEnd = std::max(0.001f, sizeEnd);
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
        ImGui::SameLine();
        if (ImGui::Button("Delete Emitter"))
        {
            levelInst.DeleteParticleEmitter(level, scene, st.selectedParticleEmitter);
            st.selectedParticleEmitter = -1;
            st.prevSelectedParticleEmitter = -2;
        }
    }

    static void DrawLightSelectionInspector(rendern::Scene& scene, LevelEditorUIState& st)
    {
        scene.EditorSanitizeLightSelection(scene.lights.size());
        st.selectedNode = -1;
        st.selectedParticleEmitter = -1;
        st.prevSelectedNode = -2;
        st.prevSelectedParticleEmitter = -2;

        ImGui::SeparatorText("Light");
        rendern::ui::DrawLightInspectorDetails(scene);
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

        scene.EditorSanitizeLightSelection(scene.lights.size());
        if (scene.editorSelectedLight >= 0)
        {
            DrawLightSelectionInspector(scene, st);
            return;
        }

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
            ImGui::TextDisabled("No node, light, or emitter selected.");
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
