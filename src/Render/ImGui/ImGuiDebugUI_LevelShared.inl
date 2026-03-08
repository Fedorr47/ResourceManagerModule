namespace rendern::ui::level_ui_detail
{
    struct LevelEditorUIState
    {
        int selectedNode = -1;
        int prevSelectedNode = -2;
        int selectedParticleEmitter = -1;
        int prevSelectedParticleEmitter = -2;
        bool addAsChildOfSelection = false;

        char nameBuf[128]{};
        char importPathBuf[512]{};

        char savePathBuf[512]{};
        char saveStatusBuf[512]{};
        std::string cachedSourcePath;
        bool saveStatusIsError = false;
    };

    struct DerivedLists
    {
        std::vector<std::vector<int>> children;
        std::vector<int> roots;
        std::vector<std::string> meshIds;
        std::vector<std::string> materialIds;
    };

    static LevelEditorUIState& GetState()
    {
        static LevelEditorUIState s{};
        return s;
    }

    static bool NodeAlive(const rendern::LevelAsset& level, int idx)
    {
        if (idx < 0)
            return false;
        const std::size_t i = static_cast<std::size_t>(idx);
        return i < level.nodes.size() && level.nodes[i].alive;
    }

    static void SyncSavePathWithSource(rendern::LevelAsset& level, LevelEditorUIState& st)
    {
        if (st.cachedSourcePath != level.sourcePath)
        {
            st.cachedSourcePath = level.sourcePath;
            const std::string fallback = st.cachedSourcePath.empty()
                ? std::string("levels/edited.level.json")
                : st.cachedSourcePath;
            std::snprintf(st.savePathBuf, sizeof(st.savePathBuf), "%s", fallback.c_str());
        }
    }

    static void BuildDerivedLists(const rendern::LevelAsset& level, DerivedLists& out)
    {
        const std::size_t ncount = level.nodes.size();
        out.children.clear();
        out.children.resize(ncount);
        out.roots.clear();
        out.roots.reserve(ncount);

        for (std::size_t i = 0; i < ncount; ++i)
        {
            const auto& n = level.nodes[i];
            if (!n.alive) continue;
            if (n.parent < 0) continue;
            if (!NodeAlive(level, n.parent)) continue;
            out.children[static_cast<std::size_t>(n.parent)].push_back(static_cast<int>(i));
        }

        for (std::size_t i = 0; i < ncount; ++i)
        {
            const auto& n = level.nodes[i];
            if (!n.alive) continue;
            if (n.parent < 0 || !NodeAlive(level, n.parent))
                out.roots.push_back(static_cast<int>(i));
        }

        out.meshIds.clear();
        out.meshIds.reserve(level.meshes.size());
        for (const auto& [id, _] : level.meshes) out.meshIds.push_back(id);
        std::sort(out.meshIds.begin(), out.meshIds.end());

        out.materialIds.clear();
        out.materialIds.reserve(level.materials.size());
        for (const auto& [id, _] : level.materials) out.materialIds.push_back(id);
        std::sort(out.materialIds.begin(), out.materialIds.end());
    }

    static std::string SanitizeId(std::string s)
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
    }

    static std::string MakeUniqueMeshId(const rendern::LevelAsset& level, std::string base)
    {
        std::string id = SanitizeId(std::move(base));
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
    }

    static void EnsureDefaultMesh(rendern::LevelAsset& level, std::string_view id, std::string_view relPath)
    {
        if (!level.meshes.contains(std::string(id)))
        {
            rendern::LevelMeshDef def{};
            def.path = std::string(relPath);
            def.debugName = std::string(id);
            level.meshes.emplace(std::string(id), std::move(def));
        }
    }

    static rendern::Transform ComputeSpawnTransform(const rendern::Scene& scene, const rendern::CameraController& camCtl)
    {
        rendern::Transform t{};
        t.position = scene.camera.position + camCtl.Forward() * 5.0f;
        t.rotationDegrees = mathUtils::Vec3(0.0f, 0.0f, 0.0f);
        t.scale = mathUtils::Vec3(1.0f, 1.0f, 1.0f);
        return t;
    }

    static int ParentForNewNode(const rendern::LevelAsset& level, const LevelEditorUIState& st)
    {
        return (st.addAsChildOfSelection && NodeAlive(level, st.selectedNode)) ? st.selectedNode : -1;
    }

    static bool ParticleEmitterAlive(const rendern::LevelAsset& level, int idx)
    {
        return idx >= 0 && static_cast<std::size_t>(idx) < level.particleEmitters.size();
    }
}
