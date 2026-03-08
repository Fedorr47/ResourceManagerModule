namespace rendern::ui::level_ui_detail
{
    static void SaveLevelToPath(
        rendern::LevelAsset& level,
        rendern::Scene& scene,
        LevelEditorUIState& st,
        const std::string& path)
    {
        try
        {
            level.camera = scene.camera;
            level.lights = scene.lights;

            rendern::SaveLevelAssetToJson(path, level);
            level.sourcePath = path;
            st.cachedSourcePath = path;
            std::snprintf(st.saveStatusBuf, sizeof(st.saveStatusBuf), "Saved: %s", path.c_str());
            st.saveStatusIsError = false;
        }
        catch (const std::exception& e)
        {
            std::snprintf(st.saveStatusBuf, sizeof(st.saveStatusBuf), "Save failed: %s", e.what());
            st.saveStatusIsError = true;
        }
    }

    static void DrawFilePanel(
        rendern::LevelAsset& level,
        rendern::Scene& scene,
        LevelEditorUIState& st)
    {
        if (!ImGui::CollapsingHeader("File", ImGuiTreeNodeFlags_DefaultOpen))
            return;

        ImGui::InputText("Level path", st.savePathBuf, sizeof(st.savePathBuf));

        const bool canHotkey = !ImGui::GetIO().WantTextInput;
        const bool ctrlS = canHotkey && ImGui::IsKeyDown(ImGuiKey_ModCtrl) && ImGui::IsKeyPressed(ImGuiKey_S);

        const std::string pathStr = std::string(st.savePathBuf);
        bool clickedSave = ImGui::Button("Save (Ctrl+S)");
        ImGui::SameLine();
        bool clickedSaveAs = ImGui::Button("Save As");

        if (ctrlS || clickedSave)
        {
            const std::string usePath = !level.sourcePath.empty() ? level.sourcePath : pathStr;
            if (!usePath.empty())
            {
                SaveLevelToPath(level, scene, st, usePath);
            }
            else
            {
                std::snprintf(st.saveStatusBuf, sizeof(st.saveStatusBuf), "Save failed: empty path");
                st.saveStatusIsError = true;
            }
        }
        else if (clickedSaveAs)
        {
            if (!pathStr.empty())
            {
                SaveLevelToPath(level, scene, st, pathStr);
            }
            else
            {
                std::snprintf(st.saveStatusBuf, sizeof(st.saveStatusBuf), "Save failed: empty path");
                st.saveStatusIsError = true;
            }
        }

        if (st.saveStatusBuf[0] != '\0')
        {
            if (st.saveStatusIsError)
                ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", st.saveStatusBuf);
            else
                ImGui::Text("%s", st.saveStatusBuf);
        }
    }
}