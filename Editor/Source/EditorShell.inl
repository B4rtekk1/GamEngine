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

namespace {

// The main-menu entries are navigation controls, not regular action buttons.
// Give them a larger target and a clearly visible hover/open state while
// keeping the rest of the editor's button styling unchanged.
bool beginTopMenu(const char *label, const char *tooltip) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {11.0F, 7.0F});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {3.0F, 0.0F});
    ImGui::PushStyleColor(ImGuiCol_Header, {0.0F, 0.0F, 0.0F, 0.0F});
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, {0.10F, 0.36F, 0.48F, 0.88F});
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, {0.08F, 0.52F, 0.66F, 1.0F});

    const bool open = ImGui::BeginMenu(label);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && tooltip != nullptr) {
        ImGui::SetTooltip("%s", tooltip);
    }
    if (open) return true;

    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
    return false;
}

void endTopMenu() {
    ImGui::EndMenu();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
}

} // namespace

Engine::Entity drawEditorMenuBar(Engine::ScenePreset &scene, Engine::Renderer &renderer,
                                 Engine::Assets::Content& content, Engine::Project& project,
                                 bool &antialiasingChanged, bool &sceneLoaded, bool &sceneSaved,
                                 const bool playing, const bool paused, bool &playToggleRequested,
                                 bool &pauseToggleRequested, const bool canUndo,
                                 const bool canRedo, const bool canPaste,
                                 bool &undoRequested, bool &redoRequested,
                                 bool &copyRequested, bool &pasteRequested,
                                 bool &duplicateRequested, bool &resetHistoryRequested,
                                 bool &showHierarchy, bool &showViewport,
                                 bool &showInspector, bool &showAssetManager,
                                 bool &showTerrainTools, bool &showConsole) {
    static bool showShortcuts = false;
    static bool showAbout = false;
    static bool openSceneSettings = false;
    static int antialiasingType = -1;
    static int msaaSamples = -1;
    static std::string sceneFileError;
    Engine::Entity createdEntity = Engine::NullEntity;
    sceneSaved = false;

    const auto saveScene = [&](const std::filesystem::path &path) {
        if (!path.parent_path().empty()) {
            std::filesystem::create_directories(path.parent_path());
        }
        const auto samples = renderer.antialiasingLevel() == Engine::AntialiasingLevel::MSAA2x
                                 ? 2u
                                 : renderer.antialiasingLevel() == Engine::AntialiasingLevel::MSAA4x
                                       ? 4u
                                       : 0u;
        Engine::SceneSerializer::save(scene, path, samples);
        EditorSceneSession::markSceneSaved(path);
        sceneSaved = true;
        sceneFileError.clear();
        Editor::ConsolePanel::info("Saved scene: " + path.string());
    };
    const auto saveSceneAs = [&] {
        if (const auto path = EditorSceneSession::chooseSaveScenePath()) {
            try {
                saveScene(*path);
            } catch (const std::exception &error) {
                sceneFileError = error.what();
                Editor::ConsolePanel::error("Could not save scene: " + sceneFileError);
            }
        }
    };
    const auto loadScene = [&](const std::filesystem::path& path) {
        std::optional<std::uint32_t> samples;
        Engine::SceneSerializer::load(scene, path, samples);
        EditorSceneSession::markSceneSaved(path);
        if (samples) {
            renderer.setAntialiasingLevel(*samples == 2
                                              ? Engine::AntialiasingLevel::MSAA2x
                                              : *samples == 4 ? Engine::AntialiasingLevel::MSAA4x
                                                              : Engine::AntialiasingLevel::Off);
            antialiasingChanged = true;
        }
        sceneLoaded = true;
        resetHistoryRequested = true;
        sceneFileError.clear();
        Editor::ConsolePanel::info("Loaded scene: " + path.string());
    };
    const auto tryLoadScene = [&](const std::filesystem::path& path) {
        try {
            loadScene(path);
        } catch (const std::exception& error) {
            sceneFileError = error.what();
            Editor::ConsolePanel::error("Could not load scene: " + sceneFileError);
        }
    };
    const auto createScene = [&] {
        const auto path = EditorSceneSession::chooseSaveScenePath();
        if (!path) return;
        try {
            if (!path->parent_path().empty()) {
                std::filesystem::create_directories(path->parent_path());
            }
            Engine::ScenePreset emptyScene;
            Engine::SceneSerializer::save(emptyScene, *path,
                                          EditorSceneSession::msaaSampleCount(renderer));
            Engine::SceneSerializer::load(scene, *path);
            EditorSceneSession::markSceneSaved(*path);
            sceneSaved = true;
            sceneFileError.clear();
            sceneLoaded = true;
            resetHistoryRequested = true;
            Editor::ConsolePanel::info("Created scene: " + path->string());
        } catch (const std::exception& error) {
            sceneFileError = error.what();
            Editor::ConsolePanel::error("Could not create scene: " + sceneFileError);
        }
    };

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
    const std::filesystem::path activeScenePath = EditorSceneSession::scenePath();
    if (beginTopMenu("File", "Project and scene files")) {
        ImGui::BeginDisabled(playing);
        if (ImGui::MenuItem("New Scene...", "Ctrl+N")) {
            createScene();
        }
        if (ImGui::MenuItem("Save Scene", "Ctrl+S")) {
            try {
                if (EditorSceneSession::hasSavedScene()) {
                    saveScene(activeScenePath);
                } else {
                    saveSceneAs();
                }
            } catch (const std::exception &error) {
                sceneFileError = error.what();
                Editor::ConsolePanel::error("Could not save scene: " + sceneFileError);
            }
        }
        if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S")) {
            saveSceneAs();
        }
        if (ImGui::MenuItem("Open Scene...", "Ctrl+Alt+O")) {
            if (const auto path = EditorSceneSession::chooseLoadScenePath()) {
                tryLoadScene(*path);
            }
        }
        if (ImGui::BeginMenu("Project Scenes")) {
            const auto scenes = project.scenes();
            if (scenes.empty()) {
                ImGui::TextDisabled("No scenes in %s", project.startupScene().parent_path().string().c_str());
            }
            for (const auto& path : scenes) {
                const std::string label = path.lexically_relative(project.rootPath()).string();
                const bool active = path == activeScenePath;
                if (ImGui::MenuItem(label.c_str(), nullptr, active, !active)) {
                    tryLoadScene(path);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Load Project...", "Ctrl+O")) {
            if (const auto manifestPath = EditorSceneSession::chooseLoadProjectPath()) {
                try {
                    Engine::Project loadedProject = Engine::Project::load(*manifestPath);
                    loadScene(loadedProject.startupScene());
                    content.clear();
                    content.setAssetRoot(loadedProject.assetRoot());
                    project = std::move(loadedProject);
                    EditorSceneSession::setProjectRoot(project.rootPath());
                    Editor::ConsolePanel::info("Loaded project: " + project.name());
                } catch (const std::exception &error) {
                    sceneFileError = error.what();
                    Editor::ConsolePanel::error("Could not load scene: " + sceneFileError);
                }
            }
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
        ImGui::Separator();
        ImGui::TextDisabled("Auto-save: every 30 seconds after changes");
        endTopMenu();
    }

    // Keep scene navigation in the main bar: switching levels should not
    // require opening File and then a nested project-scene menu.
    ImGui::SameLine(0.0F, 10.0F);
    ImGui::TextDisabled("Scene:");
    ImGui::SameLine(0.0F, 5.0F);
    ImGui::BeginDisabled(playing);
    ImGui::SetNextItemWidth(240.0F);
    const std::string activeSceneName = activeScenePath.filename().string();
    if (ImGui::BeginCombo("##active-project-scene", activeSceneName.c_str())) {
        for (const auto& path : project.scenes()) {
            const std::string label = path.lexically_relative(project.rootPath()).string();
            const bool active = path == activeScenePath;
            if (ImGui::Selectable(label.c_str(), active) && !active) {
                tryLoadScene(path);
            }
            if (active) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Switch scenes in the current project");
    }

    if (beginTopMenu("GameObject", "Create objects in the current scene")) {
        ImGui::BeginDisabled(playing);
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
        if (ImGui::MenuItem("Create Capsule")) {
            createdEntity = scene.createCapsule();
        }
        if (ImGui::MenuItem("Create Ramp")) {
            createdEntity = scene.createRamp();
        }
        if (ImGui::MenuItem("Create Light")) {
            createdEntity = scene.createLight();
        }
        if (ImGui::MenuItem("Create Terrain")) {
            createdEntity = scene.createTerrain();
        }
        if (ImGui::MenuItem("Create Procedural Cloud")) {
            createdEntity = scene.createProceduralCloud();
        }
        ImGui::EndDisabled();
        endTopMenu();
    }

    if (beginTopMenu("Scene", "Scene and rendering settings")) {
        if (ImGui::MenuItem("Antialiasing...")) {
            openSceneSettings = true;
        }
        endTopMenu();
    }

    if (beginTopMenu("View", "Show, hide and arrange editor panels")) {
        ImGui::MenuItem("Hierarchy", nullptr, &showHierarchy);
        ImGui::MenuItem("Viewport", nullptr, &showViewport);
        ImGui::MenuItem("Inspector", nullptr, &showInspector);
        ImGui::MenuItem("Asset Manager", nullptr, &showAssetManager);
        ImGui::MenuItem("Terrain Tools", nullptr, &showTerrainTools);
        ImGui::MenuItem("Console", nullptr, &showConsole);
        endTopMenu();
    }

    if (beginTopMenu("Edit", "Undo and common object actions")) {
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
        endTopMenu();
    }

    if (beginTopMenu("Help", "Shortcuts and editor information")) {
        if (ImGui::MenuItem("Keyboard Shortcuts")) {
            showShortcuts = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("About GamEngine Editor")) {
            showAbout = true;
        }
        endTopMenu();
    }

    ImGui::EndMainMenuBar();

    if (openSceneSettings) {
        if (antialiasingType < 0) {
            antialiasingType = renderer.antialiasingLevel() == Engine::AntialiasingLevel::Off ? 0 :
                                renderer.antialiasingLevel() == Engine::AntialiasingLevel::TAA ? 3 : 1;
            msaaSamples = renderer.antialiasingLevel() == Engine::AntialiasingLevel::MSAA2x ? 2 : 4;
        }
        ImGui::OpenPopup("Scene Settings");
        openSceneSettings = false;
    }

    if (ImGui::BeginPopupModal("Scene Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        constexpr const char *typeLabels[] = {"None", "MSAA", "FXAA (placeholder)", "TAA"};
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
                    } else if (antialiasingType == 3) {
                        renderer.setAntialiasingLevel(Engine::AntialiasingLevel::TAA);
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
        } else if (antialiasingType == 2) {
            ImGui::BeginDisabled();
            float placeholderValue = 1.0F;
            Editor::Controls::sliderFloat("Quality", &placeholderValue, 0.0F, 1.0F);
            ImGui::EndDisabled();
            ImGui::TextDisabled("Placeholder: this antialiasing type is not implemented yet.");
        } else if (antialiasingType == 3) {
            ImGui::TextDisabled("TAA uses jittered HDR history and resets after changes to this setting.");
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
        ImGui::BulletText("Gizmos: W move, E rotate, R scale");
        ImGui::BulletText("Hold Ctrl while transforming: position 0.25, rotation 15°, scale 0.1");
        ImGui::BulletText("Ctrl + move also snaps nearby mesh faces and vertices together");
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
