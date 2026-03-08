namespace rendern::ui::level_ui_detail
{
    static void SetSaveStatus(LevelEditorUIState& st, const std::string& message, bool isError)
    {
        std::snprintf(st.saveStatusBuf, sizeof(st.saveStatusBuf), "%s", message.c_str());
        st.saveStatusIsError = isError;
    }

    static void DrawFilePanel(
        rendern::LevelAsset& level,
        rendern::Scene& scene [[maybe_unused]],
        LevelEditorUIState& st)
    {
        ImGui::SeparatorText("Level File");
        ImGui::InputText("Save Path", st.savePathBuf, sizeof(st.savePathBuf));

        if (ImGui::Button("Save Level JSON"))
        {
            try
            {
                rendern::SaveLevelAssetToJson(st.savePathBuf, level);
                level.sourcePath = st.savePathBuf;
                st.cachedSourcePath = level.sourcePath;
                SetSaveStatus(st, std::string("Saved: ") + st.savePathBuf, false);
            }
            catch (const std::exception& e)
            {
                SetSaveStatus(st, std::string("Save failed: ") + e.what(), true);
            }
            catch (...)
            {
                SetSaveStatus(st, "Save failed: unknown error", true);
            }
        }

        if (st.saveStatusBuf[0] != '\0')
        {
            if (st.saveStatusIsError)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", st.saveStatusBuf);
            }
            else
            {
                ImGui::TextColored(ImVec4(0.35f, 1.0f, 0.45f, 1.0f), "%s", st.saveStatusBuf);
            }
        }

        ImGui::Spacing();
    }
}
