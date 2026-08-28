void drawStatusBar(const Engine::ScenePreset &scene, const Engine::Entity selected,
                   const bool playing, const bool paused) {
    ImGuiViewport *viewport = ImGui::GetMainViewport();
    const float barHeight = static_cast<float>(EditorConstants::statusBarHeight);
    ImGui::SetNextWindowPos({viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - barHeight});
    ImGui::SetNextWindowSize({viewport->WorkSize.x, barHeight});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, {0.086F, 0.090F, 0.110F, 1.0F});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {14.0F, 6.0F});
    ImGui::Begin("##status-bar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoNav);
    const ImVec4 statusColor = playing
                                   ? (paused
                                          ? ImVec4{0.95F, 0.72F, 0.32F, 1.0F}
                                          : ImVec4{0.42F, 0.86F, 0.55F, 1.0F})
                                   : ImVec4{0.42F, 0.68F, 0.92F, 1.0F};
    ImGui::TextColored(statusColor, "●");
    ImGui::SameLine(0.0F, 8.0F);
    ImGui::TextUnformatted(playing ? (paused ? "Paused" : "Playing") : "Ready");
    ImGui::SameLine(0.0F, 12.0F);
    ImGui::TextDisabled("Scene editor");
    ImGui::SameLine(0.0F, 10.0F);
    ImGui::TextDisabled("·");
    ImGui::SameLine(0.0F, 10.0F);
    ImGui::TextDisabled("%zu entities", scene.editor().size());
    ImGui::SameLine(0.0F, 10.0F);
    ImGui::TextDisabled("·");
    ImGui::SameLine(0.0F, 10.0F);
    ImGui::TextDisabled("Selected: %s",
                        selected == Engine::NullEntity ? "None" : entityName(scene, selected));
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

Engine::Entity drawEditorMenuBar(Engine::ScenePreset &scene, Engine::Renderer &renderer,
                                 bool &antialiasingChanged, bool &sceneLoaded,
                                 const bool playing, const bool paused, bool &playToggleRequested,
                                 bool &pauseToggleRequested, const bool canUndo,
                                 const bool canRedo, const bool canPaste,
                                 bool &undoRequested, bool &redoRequested,
                                 bool &copyRequested, bool &pasteRequested,
                                 bool &duplicateRequested, bool &resetHistoryRequested) {
    static bool showShortcuts = false;
    static bool showAbout = false;
    static bool openSceneSettings = false;
    static int antialiasingType = -1;
    static int msaaSamples = -1;
    static std::string sceneFileError;
    Engine::Entity createdEntity = Engine::NullEntity;

    if (!ImGui::BeginMainMenuBar()) { return Engine::NullEntity;
}

    ImGui::PushStyleColor(ImGuiCol_Text, {0.55F, 0.80F, 1.0F, 1.0F});
    ImGui::TextUnformatted("GamEngine");
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0F, 6.0F);
    ImGui::TextDisabled("Editor");
    ImGui::SameLine(0.0F, 12.0F);
    ImGui::TextDisabled("|");
    ImGui::SameLine(0.0F, 4.0F);
    if (ImGui::BeginMenu("File")) {
        const std::filesystem::path scenePath = EditorSceneSession::scenePath();
        ImGui::BeginDisabled(playing);
        if (ImGui::MenuItem("Save Scene", "Ctrl+S")) {
            try {
                std::filesystem::create_directories(scenePath.parent_path());
                const auto samples = renderer.antialiasingLevel() == Engine::AntialiasingLevel::MSAA2x
                                         ? 2u
                                         : renderer.antialiasingLevel() == Engine::AntialiasingLevel::MSAA4x
                                               ? 4u
                                               : 0u;
                Engine::SceneSerializer::save(scene, scenePath, samples);
                sceneFileError.clear();
            } catch (const std::exception &error) { sceneFileError = error.what(); }
        }
        if (ImGui::MenuItem("Load Scene", "Ctrl+O")) {
            try {
                std::optional<std::uint32_t> samples;
                Engine::SceneSerializer::load(scene, scenePath, samples);
                if (samples) {
                    renderer.setAntialiasingLevel(*samples == 2
                                                      ? Engine::AntialiasingLevel::MSAA2x
                                                      : *samples == 4
                                                            ? Engine::AntialiasingLevel::MSAA4x
                                                            : Engine::AntialiasingLevel::Off);
                    antialiasingChanged = true;
                }
                sceneLoaded = true;
                resetHistoryRequested = true;
                sceneFileError.clear();
            } catch (const std::exception &error) { sceneFileError = error.what(); }
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        if (ImGui::MenuItem(playing ? "Stop" : "Play", "F5")) {
            playToggleRequested = true;
        }
        if (ImGui::MenuItem(paused ? "Resume" : "Pause", "F6", false, playing)) {
            pauseToggleRequested = true;
        }
        if (!sceneFileError.empty()) {
            ImGui::TextDisabled("%s", sceneFileError.c_str());
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("GameObject")) {
        if (ImGui::MenuItem("Create Empty", "Ctrl+Shift+N")) {
            createdEntity = scene.createGameObject();
        }
        if (ImGui::MenuItem("Create Cube")) {
            createdEntity = scene.createCube();
        }
        if (ImGui::MenuItem("Create Plane")) {
            createdEntity = scene.createPlane();
        }
        if (ImGui::MenuItem("Create Sphere")) {
            createdEntity = scene.createSphere();
        }
        if (ImGui::MenuItem("Create Ramp")) {
            createdEntity = scene.createRamp();
        }
        if (ImGui::MenuItem("Create Terrain")) {
            createdEntity = scene.createTerrain();
        }
        ImGui::EndMenu();
    }

    // Keep the most common action visible even when the File menu is closed.
    ImGui::SameLine(0.0F, 10.0F);
    if (playing) {
        ImGui::PushStyleColor(ImGuiCol_Button, {0.62F, 0.24F, 0.24F, 1.0F});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.76F, 0.30F, 0.30F, 1.0F});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.52F, 0.18F, 0.18F, 1.0F});
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, {0.18F, 0.48F, 0.32F, 1.0F});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.24F, 0.60F, 0.40F, 1.0F});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.14F, 0.40F, 0.26F, 1.0F});
    }
    if (EditorButton(playing ? "  Stop  " : "  Play  ").draw()) {
        playToggleRequested = true;
    }
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(playing
                              ? "Stop play mode and restore the editor scene (F5)"
                              : "Run the current scene in Game View (F5)");
    }
    ImGui::SameLine(0.0F, 6.0F);
    ImGui::BeginDisabled(!playing);
    if (paused) {
        ImGui::PushStyleColor(ImGuiCol_Button, {0.55F, 0.42F, 0.16F, 1.0F});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.68F, 0.52F, 0.20F, 1.0F});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.45F, 0.34F, 0.12F, 1.0F});
    }
    if (EditorButton(paused ? "  Resume  " : "  Pause  ").draw()) {
        pauseToggleRequested = true;
    }
    if (paused) {
        ImGui::PopStyleColor(3);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Pause or resume script updates (F6)");
    }

    if (ImGui::BeginMenu("Scene")) {
        if (ImGui::MenuItem("Antialiasing...")) {
            openSceneSettings = true;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canUndo)) {
            undoRequested = true;
        }
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, canRedo)) {
            redoRequested = true;
        }
        ImGui::Separator();
        ImGui::MenuItem("Cut", "Ctrl+X", false, false);
        if (ImGui::MenuItem("Copy", "Ctrl+C")) {
            copyRequested = true;
        }
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, canPaste)) {
            pasteRequested = true;
        }
        if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
            duplicateRequested = true;
        }
        ImGui::Separator();
        ImGui::MenuItem("Select All", "Ctrl+A", false, false);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Keyboard Shortcuts")) {
            showShortcuts = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("About GamEngine Editor")) {
            showAbout = true;
        }
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();

    if (openSceneSettings) {
        if (antialiasingType < 0) {
            antialiasingType = renderer.antialiasingLevel() == Engine::AntialiasingLevel::Off ? 0 : 1;
            msaaSamples = renderer.antialiasingLevel() == Engine::AntialiasingLevel::MSAA2x ? 2 : 4;
        }
        ImGui::OpenPopup("Scene Settings");
        openSceneSettings = false;
    }

    if (ImGui::BeginPopupModal("Scene Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        constexpr const char *typeLabels[] = {"None", "MSAA", "FXAA (placeholder)", "TAA (placeholder)"};
        constexpr const char *sampleLabels[] = {"2x", "4x"};

        ImGui::TextUnformatted("Antialiasing");
        ImGui::Separator();
        ImGui::SetNextItemWidth(230.0F);
        if (ImGui::BeginCombo("Type", typeLabels[antialiasingType])) {
            for (int index = 0; index < 4; ++index) {
                const bool isSelected = antialiasingType == index;
                if (ImGui::Selectable(typeLabels[index], isSelected)) {
                    antialiasingType = index;
                    if (antialiasingType == 0) {
                        renderer.setAntialiasingLevel(Engine::AntialiasingLevel::Off);
                        antialiasingChanged = true;
                    } else if (antialiasingType == 1) {
                        renderer.setAntialiasingLevel(msaaSamples == 2
                                                          ? Engine::AntialiasingLevel::MSAA2x
                                                          : Engine::AntialiasingLevel::MSAA4x);
                        antialiasingChanged = true;
                    }
                }
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        ImGui::Spacing();
        if (antialiasingType == 1) {
            ImGui::SetNextItemWidth(230.0F);
            if (ImGui::BeginCombo("Samples", msaaSamples == 2 ? sampleLabels[0] : sampleLabels[1])) {
                for (const int samples: {2, 4}) {
                    const bool isSelected = msaaSamples == samples;
                    if (ImGui::Selectable(samples == 2 ? sampleLabels[0] : sampleLabels[1], isSelected)) {
                        msaaSamples = samples;
                        renderer.setAntialiasingLevel(samples == 2
                                                          ? Engine::AntialiasingLevel::MSAA2x
                                                          : Engine::AntialiasingLevel::MSAA4x);
                        antialiasingChanged = true;
                    }
                    if (isSelected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::TextDisabled("MSAA is currently supported by the renderer.");
        } else if (antialiasingType == 2 || antialiasingType == 3) {
            ImGui::BeginDisabled();
            float placeholderValue = 1.0F;
            ImGui::SliderFloat("Quality", &placeholderValue, 0.0F, 1.0F);
            ImGui::EndDisabled();
            ImGui::TextDisabled("Placeholder: this antialiasing type is not implemented yet.");
        } else {
            ImGui::TextDisabled("Antialiasing is disabled.");
        }

        ImGui::Separator();
        ImGui::TextDisabled("Changes are applied after reloading the scene.");
        if (EditorButton("Close").draw()) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    if (showShortcuts) {
        ImGui::Begin("Keyboard Shortcuts", &showShortcuts);
        ImGui::TextUnformatted("Editor shortcuts");
        ImGui::Separator();
        ImGui::BulletText("Scene View: hold RMB to look; RMB + WASD moves");
        ImGui::BulletText("Scene View: Q / E down/up, Shift speeds up, MMB pans, wheel zooms");
        ImGui::BulletText("Delete: remove selected object");
        ImGui::BulletText("Ctrl+C / Ctrl+V: copy and paste selected object");
        ImGui::BulletText("Ctrl+D: duplicate selected object");
        ImGui::BulletText("Ctrl+Z / Ctrl+Y: undo / redo scene edits");
        ImGui::End();
    }

    if (showAbout) {
        ImGui::Begin("About GamEngine Editor", &showAbout);
        ImGui::TextUnformatted("GamEngine Editor");
        ImGui::TextUnformatted("A lightweight scene and particle editor.");
        ImGui::Separator();
        ImGui::TextUnformatted("Unity-inspired workspace");
        ImGui::End();
    }

    return createdEntity;
}
