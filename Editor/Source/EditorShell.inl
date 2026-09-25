void drawStatusBar(const Engine::ScenePreset &scene, const Engine::Entity selected,
                   const bool playing, const bool paused) {
    ImGuiViewport *viewport = ImGui::GetMainViewport();
    const float barHeight = static_cast<float>(EditorConstants::statusBarHeight);
    ImGui::SetNextWindowPos({viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - barHeight});
    ImGui::SetNextWindowSize({viewport->WorkSize.x, barHeight});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorUI::colors().surface);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {14.0F, 6.0F});
    ImGui::Begin("##status-bar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoNav);
    const ImVec4 statusColor = playing
                                   ? (paused ? EditorUI::colors().warning : EditorUI::colors().success)
                                   : EditorUI::colors().accent;
    ImGui::TextColored(statusColor, "●");
    ImGui::SameLine(0.0F, 8.0F);
    ImGui::TextUnformatted(playing ? (paused ? "Paused" : "Playing") : "Ready");
    ImGui::SameLine(0.0F, 12.0F);
    ImGui::TextDisabled("Scene editor");
    ImGui::SameLine(0.0F, 10.0F);
    ImGui::TextDisabled("·");
    ImGui::SameLine(0.0F, 10.0F);
    ImGui::TextDisabled("%zu entities", scene.view().size());
    ImGui::SameLine(0.0F, 10.0F);
    ImGui::TextDisabled("·");
    ImGui::SameLine(0.0F, 10.0F);
    ImGui::TextDisabled("Selected: %s",
                        selected == Engine::NullEntity ? "None" : entityName(scene, selected));
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

[[maybe_unused]] void drawLegacyGpuProfilePanel(const Engine::Renderer &renderer, bool &isOpen) {
    (void) renderer;
    static bool writeCsv{};
    static float intervalSeconds{2.0F};
    static std::chrono::steady_clock::time_point lastWrite{};
    static std::string writeError;
    const std::filesystem::path csvPath = Platform::UserPaths::editorLogs() / "gpu-profile.csv";
    const std::uint32_t historyCount = Engine::Profiler::historySize();
    const Engine::ProfileFrame *latestGpuFrame = nullptr;
    for (std::uint32_t index = historyCount; index != 0; --index) {
        const Engine::ProfileFrame &frame = Engine::Profiler::historyFrame(index - 1);
        if (frame.gpuReady) {
            latestGpuFrame = &frame;
            break;
        }
    }

    // Sampling history has no GPU synchronization cost. Keep recording active
    // while hidden, so it remains useful during uninterrupted benchmark runs.
    const auto monotonicNow = std::chrono::steady_clock::now();
    if (writeCsv && latestGpuFrame &&
        (lastWrite == std::chrono::steady_clock::time_point{} ||
         monotonicNow - lastWrite >= std::chrono::duration<float>{intervalSeconds})) {
        std::error_code error;
        std::filesystem::create_directories(csvPath.parent_path(), error);
        if (error) {
            writeError = "Could not create log directory: " + error.message();
        } else {
            const bool writeHeader = !std::filesystem::exists(csvPath, error) ||
                                     (!error && std::filesystem::file_size(csvPath, error) == 0);
            std::ofstream output{csvPath, std::ios::app};
            if (!output) {
                writeError = "Could not open GPU profile CSV for writing.";
            } else {
                if (writeHeader) {
                    output << "timestamp_unix_ms,gpu_frame_ms,profiled_zones_ms\n";
                }
                float profiledMilliseconds = 0.0F;
                for (const Engine::GpuProfileEvent &event: latestGpuFrame->gpuEvents) {
                    if (event.depth == 0) profiledMilliseconds += event.endMs - event.startMs;
                }
                const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                output << timestamp << ',' << latestGpuFrame->gpuFrameMs << ',' << profiledMilliseconds << '\n';
                writeError.clear();
                lastWrite = monotonicNow;
            }
        }
    }

    if (!isOpen) return;
    if (!ImGui::Begin("GPU Profile", &isOpen)) {
        ImGui::End();
        return;
    }

    static bool followLatest{true};
    static std::uint64_t selectedFrame{};
    if (ImGui::Button(followLatest ? "Lock frame" : "Follow latest")) followLatest = !followLatest;
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        Engine::Profiler::clear();
        selectedFrame = 0;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Write CSV", &writeCsv)) lastWrite = {};
    ImGui::SameLine();
    ImGui::SetNextItemWidth(88.0F);
    if (ImGui::DragFloat("##gpu-profile-interval", &intervalSeconds, 0.25F, 0.25F, 60.0F,
                         "every %.2f s")) {
        lastWrite = {};
    }
    if (writeCsv) ImGui::TextDisabled("%s", csvPath.string().c_str());
    if (!writeError.empty()) ImGui::TextColored(EditorUI::colors().error, "%s", writeError.c_str());
    ImGui::Separator();
    if (historyCount == 0) {
        ImGui::TextDisabled("Waiting for profiled frames...");
        ImGui::End();
        return;
    }

    if (historyCount != 0) {
        if (followLatest || selectedFrame == 0)
            selectedFrame = Engine::Profiler::historyFrame(historyCount - 1).frameNumber;
        std::array<float, Engine::Profiler::HistorySize> cpuTimes{};
        std::array<float, Engine::Profiler::HistorySize> gpuTimes{};
        float maxCpu = 0.0F;
        float maxGpu = 0.0F;
        for (std::uint32_t index = 0; index < historyCount; ++index) {
            const Engine::ProfileFrame &frame = Engine::Profiler::historyFrame(index);
            cpuTimes[index] = static_cast<float>(frame.cpuFrameMs);
            gpuTimes[index] = static_cast<float>(frame.gpuFrameMs);
            maxCpu = std::max(maxCpu, cpuTimes[index]);
            maxGpu = std::max(maxGpu, gpuTimes[index]);
        }
        const float graphMax = std::max({16.667F, maxCpu, maxGpu});
        ImGui::TextUnformatted("Frame time history");
        ImGui::PlotLines("CPU (ms)", cpuTimes.data(), static_cast<int>(historyCount), 0,
                         nullptr, 0.0F, graphMax, {0.0F, 72.0F});
        ImGui::PlotLines("GPU (ms)", gpuTimes.data(), static_cast<int>(historyCount), 0,
                         nullptr, 0.0F, graphMax, {0.0F, 72.0F});
        if (ImGui::BeginCombo("Selected frame", ("#" + std::to_string(selectedFrame)).c_str())) {
            for (std::uint32_t index = 0; index < historyCount; ++index) {
                const Engine::ProfileFrame &frame = Engine::Profiler::historyFrame(index);
                const bool selected = frame.frameNumber == selectedFrame;
                const std::string label = "#" + std::to_string(frame.frameNumber) + "  CPU " +
                                          std::to_string(frame.cpuFrameMs).substr(0, 5) + " ms";
                if (ImGui::Selectable(label.c_str(), selected)) {
                    selectedFrame = frame.frameNumber;
                    followLatest = false;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        const std::uint64_t targetFrame = selectedFrame;
        const Engine::ProfileFrame *selected = nullptr;
        for (std::uint32_t index = 0; index < historyCount; ++index) {
            const Engine::ProfileFrame &frame = Engine::Profiler::historyFrame(index);
            if (frame.frameNumber == targetFrame) {
                selected = &frame;
                break;
            }
        }
        if (selected != nullptr) {
            ImGui::Text("CPU %.3f ms   GPU %s", selected->cpuFrameMs,
                        selected->gpuReady ? (std::to_string(selected->gpuFrameMs) + " ms").c_str() : "pending");
            if (ImGui::BeginTable("##cpu-profile-zones", 3,
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("CPU zone");
                ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_WidthFixed, 86.0F);
                ImGui::TableSetupColumn("Self", ImGuiTableColumnFlags_WidthFixed, 86.0F);
                ImGui::TableHeadersRow();
                for (std::size_t index = 0; index < selected->cpuEvents.size(); ++index) {
                    const auto &event = selected->cpuEvents[index];
                    const double total = static_cast<double>(event.endNs - event.startNs) * 1.0e-6;
                    std::uint64_t childNs{};
                    for (std::size_t child = index + 1; child < selected->cpuEvents.size(); ++child) {
                        const auto &candidate = selected->cpuEvents[child];
                        if (candidate.depth <= event.depth) break;
                        if (candidate.depth == event.depth + 1) childNs += candidate.endNs - candidate.startNs;
                    }
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Indent(static_cast<float>(event.depth) * 12.0F);
                    ImGui::TextUnformatted(Engine::Profiler::name(event.name).data());
                    ImGui::Unindent(static_cast<float>(event.depth) * 12.0F);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%.3f ms", total);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%.3f ms", total - static_cast<double>(childNs) * 1.0e-6);
                }
                ImGui::EndTable();
            }
        }
        ImGui::Separator();
    }

    const Engine::ProfileFrame *selectedGpuFrame = nullptr;
    for (std::uint32_t index = 0; index < historyCount; ++index) {
        const Engine::ProfileFrame &frame = Engine::Profiler::historyFrame(index);
        if (frame.frameNumber == selectedFrame) {
            selectedGpuFrame = &frame;
            break;
        }
    }
    float profiledMilliseconds = 0.0F;
    if (selectedGpuFrame && selectedGpuFrame->gpuReady && ImGui::BeginTable("##gpu-profile-passes", 2,
                                                                            ImGuiTableFlags_SizingStretchProp |
                                                                            ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("GPU zone");
        ImGui::TableSetupColumn("GPU time", ImGuiTableColumnFlags_WidthFixed, 92.0F);
        ImGui::TableHeadersRow();
        for (const Engine::GpuProfileEvent &event: selectedGpuFrame->gpuEvents) {
            const float milliseconds = event.endMs - event.startMs;
            if (event.depth == 0) profiledMilliseconds += milliseconds;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Indent(static_cast<float>(event.depth) * 12.0F);
            ImGui::TextUnformatted(Engine::Profiler::name(event.name).data());
            ImGui::Unindent(static_cast<float>(event.depth) * 12.0F);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.3f ms", milliseconds);
        }
        ImGui::EndTable();
    }
    ImGui::Separator();
    if (selectedGpuFrame && selectedGpuFrame->gpuReady) {
        ImGui::Text("GPU frame: %.3f ms", selectedGpuFrame->gpuFrameMs);
        ImGui::Text("Profiled zones: %.3f ms   Unaccounted: %.3f ms", profiledMilliseconds,
                    std::max(0.0F, static_cast<float>(selectedGpuFrame->gpuFrameMs) - profiledMilliseconds));
    } else {
        ImGui::TextDisabled("GPU timestamps for this CPU frame are still pending.");
    }
    ImGui::End();
}

namespace {
    // The main-menu entries are navigation controls, not regular action buttons.
    // Give them a larger target and a clearly visible hover/open state while
    // keeping the rest of the editor's button styling unchanged.
    bool beginTopMenu(const char *label, const char *tooltip, Editor::WindowsTitleBar &titleBar) {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {11.0F, ImGui::GetStyle().FramePadding.y});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {3.0F, 0.0F});
        ImGui::PushStyleColor(ImGuiCol_Header, {0.0F, 0.0F, 0.0F, 0.0F});
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, {0.10F, 0.36F, 0.48F, 0.88F});
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, {0.08F, 0.52F, 0.66F, 1.0F});

        const bool open = ImGui::BeginMenu(label);
        const ImVec2 itemMin = ImGui::GetItemRectMin();
        const ImVec2 itemMax = ImGui::GetItemRectMax();
        titleBar.addHitRegion(itemMin.x, itemMin.y, itemMax.x, itemMax.y,
                              Editor::WindowsTitleBar::clientHitTestResult);
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

    [[nodiscard]] float pixelAligned(const float value) {
        return std::round(value) + 0.5F;
    }

    void drawRestoreGlyph(ImDrawList * const drawList, const ImVec2 center, const float glyphHalfExtent,

    const ImU32 color
    )
 {
    const float offset = std::round(glyphHalfExtent * 0.6F);
    const float halfSize = glyphHalfExtent;

    // Draw only the exposed upper and right edges of the back window.  A
    // second complete rectangle would look like two maximize glyphs.
    drawList->AddLine({pixelAligned(center.x - halfSize + offset), pixelAligned(center.y - halfSize - offset)},
                      {pixelAligned(center.x + halfSize + offset), pixelAligned(center.y - halfSize - offset)}, color);
    drawList->AddLine({pixelAligned(center.x + halfSize + offset), pixelAligned(center.y - halfSize - offset)},
                      {pixelAligned(center.x + halfSize + offset), pixelAligned(center.y + halfSize - offset)}, color);
    drawList->AddRect({pixelAligned(center.x - halfSize), pixelAligned(center.y - halfSize)},
                      {pixelAligned(center.x + halfSize), pixelAligned(center.y + halfSize)}, color);
}

} // namespace

Engine::Entity drawEditorMenuBar(Engine::ScenePreset &scene, Engine::Renderer &renderer,
                                 Engine::Assets::Content &content, Engine::Project &project,
                                 Editor::WindowsTitleBar &titleBar,
                                 bool &antialiasingChanged, bool &sceneLoaded, bool &sceneSaved,
                                 bool &sceneDeleted,
                                 const bool playing, const bool paused, bool &playToggleRequested,
                                 bool &pauseToggleRequested, const bool canUndo,
                                 const bool canRedo, const bool canPaste,
                                 bool &undoRequested, bool &redoRequested,
                                 bool &copyRequested, bool &pasteRequested,
                                 bool &duplicateRequested, bool &resetHistoryRequested,
                                 bool &showHierarchy, bool &showViewport,
                                 bool &showInspector, bool &showAssetManager,
                                 bool &showTerrainTools, bool &showConsole, bool &showTerminal,
                                 bool &showShaderGraph, bool &showGpuProfile, bool &gameBuildRequested) {
    static bool showShortcuts = false;
    static bool showAbout = false;
    static bool openSceneSettings = false;
    static bool openNewProject = false;
    static char newProjectName[128] = "MyGame";
    static char newProjectLocation[1024]{};
    static std::string newProjectError;
    static int antialiasingType = -1;
    static int msaaSamples = -1;
    static std::string sceneFileError;
    static bool openDeleteScene = false;
    Engine::Entity createdEntity = Engine::NullEntity;
    sceneSaved = false;
    sceneDeleted = false;

    const auto saveScene = [&](const std::filesystem::path &path) {
        if (!path.parent_path().empty()) {
            std::filesystem::create_directories(path.parent_path());
        }
        Engine::SceneSerializer::save(scene, path, renderer.antialiasingLevel());
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
    const auto loadScene = [&](const std::filesystem::path &path) {
        std::optional<Engine::AntialiasingLevel> antialiasing;
        Engine::SceneSerializer::load(scene, path, antialiasing);
        if (!scene.environmentEquirectangular().empty() &&
            !renderer.setEnvironmentEquirectangular(content.assetRoot() / scene.environmentEquirectangular())) {
            throw std::runtime_error("Could not load scene HDR/EXR environment");
        }
        EditorSceneSession::markSceneSaved(path);
        if (antialiasing) {
            renderer.setAntialiasingLevel(*antialiasing);
            antialiasingChanged = true;
        }
        sceneLoaded = true;
        resetHistoryRequested = true;
        sceneFileError.clear();
        Editor::ConsolePanel::info("Loaded scene: " + path.string());
    };
    const auto tryLoadScene = [&](const std::filesystem::path &path) {
        try {
            loadScene(path);
        } catch (const std::exception &error) {
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
                                          EditorSceneSession::antialiasingLevel(renderer));
            Engine::SceneSerializer::load(scene, *path);
            EditorSceneSession::markSceneSaved(*path);
            sceneSaved = true;
            sceneFileError.clear();
            sceneLoaded = true;
            resetHistoryRequested = true;
            Editor::ConsolePanel::info("Created scene: " + path->string());
        } catch (const std::exception &error) {
            sceneFileError = error.what();
            Editor::ConsolePanel::error("Could not create scene: " + sceneFileError);
        }
    };
    const auto createProject = [&] {
        try {
            const std::filesystem::path root{newProjectLocation};
            Engine::Project createdProject = Engine::Project::create(root, newProjectName);
            loadScene(createdProject.startupScene());
            content.clear();
            content.setAssetRoot(createdProject.assetRoot());
            project = std::move(createdProject);
            EditorSceneSession::setProjectRoot(project.rootPath());
            newProjectError.clear();
            Editor::ConsolePanel::info("Created project: " + project.name());
        } catch (const std::exception &error) {
            newProjectError = error.what();
            Editor::ConsolePanel::error("Could not create project: " + newProjectError);
        }
    };

    titleBar.clearHitRegions();
    const float titleHeight = static_cast<float>(titleBar.nativeHeight());
    const float fontHeight = ImGui::GetFontSize();
    const float paddingY = std::max(0.0F, (titleHeight - fontHeight) * 0.5F);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8.0F, paddingY});
    if (!ImGui::BeginMainMenuBar()) {
        ImGui::PopStyleVar();
        return Engine::NullEntity;
    }
    const float captionButtonsStart = titleBar.contentRight();
    if (captionButtonsStart > 0.0F) {
        const ImVec2 menuBarOrigin = ImGui::GetWindowPos();
        ImGui::PushClipRect(menuBarOrigin,
                            {captionButtonsStart - 8.0F, menuBarOrigin.y + ImGui::GetFrameHeight()}, true);
    }

    const std::filesystem::path activeScenePath = EditorSceneSession::scenePath();
    const std::string sceneLabel = activeScenePath.empty() ? "Untitled" : activeScenePath.filename().string();
    const std::string titleLabel = sceneLabel + " — " + (playing ? "DEMO" : "EDITOR") + " — " + project.name();
    ImVec4 titleColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) titleColor.w *= 0.62F;
    ImGui::PushStyleColor(ImGuiCol_Text, titleColor);
    ImGui::TextUnformatted(titleLabel.c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0F, 14.0F);
    if (beginTopMenu("File", "Project and scene files", titleBar)) {
        ImGui::BeginDisabled(playing);
        if (ImGui::MenuItem("New Project...")) {
            if (newProjectLocation[0] == '\0') {
                const std::string defaultLocation = (std::filesystem::current_path() / newProjectName).string();
                std::snprintf(newProjectLocation, sizeof(newProjectLocation), "%s", defaultLocation.c_str());
            }
            newProjectError.clear();
            openNewProject = true;
        }
        ImGui::Separator();
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
        if (ImGui::MenuItem("Delete Scene...", nullptr, false, EditorSceneSession::hasSavedScene())) {
            openDeleteScene = true;
        }
        if (ImGui::MenuItem("Open Scene...", "Ctrl+Alt+O")) {
            if (const auto path = EditorSceneSession::chooseLoadScenePath()) {
                tryLoadScene(*path);
            }
        }
        if (ImGui::BeginMenu("Project Scenes")) {
            const auto scenes = project.scenes();
            if (scenes.empty()) {
                ImGui::TextDisabled("No scenes in %s", (project.assetRoot() / "Scenes").string().c_str());
            }
            for (const auto &path: scenes) {
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
                    content.clear();
                    content.setAssetRoot(loadedProject.assetRoot());
                    project = std::move(loadedProject);
                    EditorSceneSession::setProjectRoot(project.rootPath());
                    if (std::filesystem::is_regular_file(project.startupScene())) {
                        loadScene(project.startupScene());
                    } else {
                        EditorSceneSession::clearSavedScene();
                        sceneDeleted = true;
                    }
                    Editor::ConsolePanel::info("Loaded project: " + project.name());
                } catch (const std::exception &error) {
                    sceneFileError = error.what();
                    Editor::ConsolePanel::error("Could not load scene: " + sceneFileError);
                }
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Build Game", "", false, !project.manifestPath().empty())) {
            gameBuildRequested = true;
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

    if (openDeleteScene)
        ImGui::OpenPopup("##confirm-delete-scene");
    if (ImGui::BeginPopupModal("##confirm-delete-scene", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Delete scene '%s'?", activeScenePath.filename().string().c_str());
        ImGui::TextDisabled("This permanently removes the scene file from the project.");
        ImGui::Separator();
        if (ImGui::Button("Delete", {120.0F, 0.0F})) {
            std::error_code error;
            if (std::filesystem::remove(activeScenePath, error) && !error) {
                const auto terrainPath = std::filesystem::path{activeScenePath.string() + ".terrain"};
                std::filesystem::remove(terrainPath, error);
                error.clear();
                const auto environmentPath = std::filesystem::path{activeScenePath.string() + ".environment"};
                std::filesystem::remove(environmentPath, error);
                EditorSceneSession::clearSavedScene();
                sceneDeleted = true;
                sceneFileError.clear();
                Editor::ConsolePanel::info("Deleted scene: " + activeScenePath.string());
            } else {
                sceneFileError = error ? error.message() : "Scene file does not exist";
                Editor::ConsolePanel::error("Could not delete scene: " + sceneFileError);
            }
            openDeleteScene = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {120.0F, 0.0F})) {
            openDeleteScene = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Keep scene navigation in the main bar: switching levels should not
    // require opening File and then a nested project-scene menu.
    ImGui::SameLine(0.0F, 10.0F);
    ImGui::TextDisabled("Scene:");
    ImGui::SameLine(0.0F, 5.0F);
    ImGui::BeginDisabled(playing);
    ImGui::SetNextItemWidth(240.0F);
    const std::string activeSceneName = activeScenePath.empty()
                                            ? "No scene selected"
                                            : activeScenePath.filename().string();
    if (ImGui::BeginCombo("##active-project-scene", activeSceneName.c_str())) {
        if (ImGui::Selectable("No scene selected", activeScenePath.empty())) {
            EditorSceneSession::clearSavedScene();
            sceneDeleted = true;
        }
        ImGui::Separator();
        for (const auto &path: project.scenes()) {
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
    const ImVec2 sceneSelectorMin = ImGui::GetItemRectMin();
    const ImVec2 sceneSelectorMax = ImGui::GetItemRectMax();
    titleBar.addHitRegion(sceneSelectorMin.x, sceneSelectorMin.y, sceneSelectorMax.x,
                          sceneSelectorMax.y, Editor::WindowsTitleBar::clientHitTestResult);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Switch scenes in the current project");
    }

    if (beginTopMenu("GameObject", "Create objects in the current scene", titleBar)) {
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
        if (ImGui::MenuItem("Create Camera")) {
            createdEntity = scene.createCamera();
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
        if (ImGui::BeginMenu("Water")) {
            if (ImGui::MenuItem("Ocean")) createdEntity = scene.createOcean();
            if (ImGui::MenuItem("Lake")) createdEntity = scene.createLake();
            if (ImGui::MenuItem("River")) createdEntity = scene.createRiver();
            ImGui::EndMenu();
        }
        ImGui::EndDisabled();
        endTopMenu();
    }

    if (beginTopMenu("Scene", "Scene and rendering settings", titleBar)) {
        if (ImGui::MenuItem("Antialiasing...")) {
            openSceneSettings = true;
        }
        if (ImGui::BeginMenu("Shadow Debug")) {
            const Engine::ShadowDebugView current = renderer.shadowDebugView();
            const auto selectDebugView = [&](const char *label, const Engine::ShadowDebugView view) {
                if (ImGui::MenuItem(label, nullptr, current == view)) renderer.setShadowDebugView(view);
            };
            selectDebugView("Off", Engine::ShadowDebugView::Off);
            ImGui::Separator();
            selectDebugView("Blocker Count", Engine::ShadowDebugView::BlockerCount);
            selectDebugView("Penumbra Radius", Engine::ShadowDebugView::PenumbraRadius);
            selectDebugView("Desired Clip Level", Engine::ShadowDebugView::DesiredClipLevel);
            selectDebugView("Resolved Clip Level", Engine::ShadowDebugView::ResolvedClipLevel);
            selectDebugView("Fallback Delta", Engine::ShadowDebugView::FallbackDelta);
            selectDebugView("Virtual Page", Engine::ShadowDebugView::VirtualPage);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("GTAO Debug")) {
            const Engine::GtaoDebugView current = renderer.gtaoDebugView();
            const auto selectGtaoDebugView = [&](const char *label, const Engine::GtaoDebugView view) {
                if (ImGui::MenuItem(label, nullptr, current == view)) renderer.setGtaoDebugView(view);
            };
            selectGtaoDebugView("Final Lighting", Engine::GtaoDebugView::Off);
            ImGui::Separator();
            selectGtaoDebugView("GTAO Raw", Engine::GtaoDebugView::Raw);
            selectGtaoDebugView("GTAO Filtered", Engine::GtaoDebugView::Filtered);
            selectGtaoDebugView("GTAO Full", Engine::GtaoDebugView::Full);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("PBR Diagnostics")) {
            const Engine::PbrDebugView current = renderer.pbrDebugView();
            const auto selectPbrDebugView = [&](const char *label, const Engine::PbrDebugView view) {
                if (ImGui::MenuItem(label, nullptr, current == view)) renderer.setPbrDebugView(view);
            };
            selectPbrDebugView("Final Lighting", Engine::PbrDebugView::FinalLighting);
            ImGui::Separator();
            selectPbrDebugView("Disable Specular IBL", Engine::PbrDebugView::NoSpecularIbl);
            selectPbrDebugView("Disable Direct Specular", Engine::PbrDebugView::NoDirectSpecular);
            selectPbrDebugView("Disable GTAO", Engine::PbrDebugView::NoGtao);
            ImGui::Separator();
            selectPbrDebugView("Geometric Direct Diffuse", Engine::PbrDebugView::GeometricDirectDiffuse);
            ImGui::Separator();
            selectPbrDebugView("DDGI Irradiance", Engine::PbrDebugView::DdgiIrradiance);
            selectPbrDebugView("DDGI Blend Weight", Engine::PbrDebugView::DdgiBlendWeight);
            selectPbrDebugView("DDGI Visibility", Engine::PbrDebugView::DdgiVisibility);
            selectPbrDebugView("DDGI Active Probes", Engine::PbrDebugView::DdgiProbeCount);
            selectPbrDebugView("DDGI Cascade", Engine::PbrDebugView::DdgiCascade);
            selectPbrDebugView("DDGI vs Sky", Engine::PbrDebugView::DdgiVsSky);
            selectPbrDebugView("DDGI Pre-Visibility Weight", Engine::PbrDebugView::DdgiPreVisibilityWeight);
            selectPbrDebugView("DDGI Post-Visibility Weight", Engine::PbrDebugView::DdgiPostVisibilityWeight);
            ImGui::EndMenu();
        }
        endTopMenu();
    }

    if (beginTopMenu("Rendering", "Quickly switch renderer quality", titleBar)) {
        constexpr std::array<const char *, 4> presetLabels{"Low", "Medium", "High", "Ultra"};
        const auto currentPreset = [&] {
            for (const auto preset : {Engine::RenderQualityPreset::Low,
                                      Engine::RenderQualityPreset::Medium,
                                      Engine::RenderQualityPreset::High,
                                      Engine::RenderQualityPreset::Ultra}) {
                const auto settings = Engine::renderQualityPresetSettings(preset);
                if (renderer.shadowQuality() == settings.shadows &&
                    renderer.gtaoQuality() == settings.gtao &&
                    renderer.antialiasingLevel() == settings.antialiasing) return preset;
            }
            return Engine::RenderQualityPreset::High;
        }();
        const int currentIndex = static_cast<int>(currentPreset);
        for (int index = 0; index < static_cast<int>(presetLabels.size()); ++index) {
            if (ImGui::MenuItem(presetLabels[static_cast<std::size_t>(index)], nullptr,
                                currentIndex == index)) {
                renderer.applyRenderQualityPreset(
                    static_cast<Engine::RenderQualityPreset>(index));
                antialiasingChanged = true;
                Editor::ConsolePanel::info(std::string{"Renderer preset: "} + presetLabels[static_cast<std::size_t>(index)]);
            }
        }
        ImGui::Separator();
        bool contactShadowsEnabled = renderer.contactShadowMode() == Engine::ContactShadowMode::RayTraced;
        if (ImGui::MenuItem("RT contact refinement (VSM)", nullptr, &contactShadowsEnabled)) {
            renderer.setContactShadowMode(contactShadowsEnabled
                ? Engine::ContactShadowMode::RayTraced
                : Engine::ContactShadowMode::Off);
        }
        if (contactShadowsEnabled) {
            ImGui::TextDisabled(renderer.contactShadowsActive()
                ? "Active in Game View"
                : "Requested; inactive in current view/frame");
            ImGui::TextDisabled("Requires Game View, VSM shadows, Ray Query and no MSAA.");
            auto settings = renderer.rtContactShadowSettings();
            bool settingsChanged = false;
            settingsChanged |= ImGui::SliderFloat("Contact distance", &settings.maxDistance, 0.05F, 2.0F, "%.2f m");
            settingsChanged |= ImGui::SliderFloat("Normal bias", &settings.normalBias, 0.0005F, 0.05F, "%.4f");
            static float resolutionScaleDraft = -1.0F;
            if (resolutionScaleDraft < 0.0F) resolutionScaleDraft = settings.resolutionScale;
            ImGui::SliderFloat("Resolution scale", &resolutionScaleDraft, 0.25F, 1.0F, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                settings.resolutionScale = resolutionScaleDraft;
                settingsChanged = true;
            } else if (!ImGui::IsItemActive()) {
                resolutionScaleDraft = settings.resolutionScale;
            }
            if (settingsChanged) renderer.setRtContactShadowSettings(settings);
        }
        ImGui::TextDisabled("Shadows apply next frame; GTAO resources are rebuilt safely.");
        endTopMenu();
    }

    if (beginTopMenu("View", "Show, hide and arrange editor panels", titleBar)) {
        ImGui::MenuItem("Hierarchy", nullptr, &showHierarchy);
        ImGui::MenuItem("Viewport", nullptr, &showViewport);
        ImGui::MenuItem("Inspector", nullptr, &showInspector);
        ImGui::MenuItem("Asset Manager", nullptr, &showAssetManager);
        ImGui::MenuItem("Terrain Tools", nullptr, &showTerrainTools);
        ImGui::MenuItem("Console", nullptr, &showConsole);
        ImGui::MenuItem("Terminal", nullptr, &showTerminal);
        ImGui::MenuItem("Shader Graph", nullptr, &showShaderGraph);
        ImGui::MenuItem("Profiler", nullptr, &showGpuProfile);
        endTopMenu();
    }

    if (beginTopMenu("Edit", "Undo and common object actions", titleBar)) {
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

    if (beginTopMenu("Help", "Shortcuts and editor information", titleBar)) {
        if (ImGui::MenuItem("Keyboard Shortcuts")) {
            showShortcuts = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("About GamEngine Editor")) {
            showAbout = true;
        }
        endTopMenu();
    }

    if (captionButtonsStart > 0.0F) ImGui::PopClipRect();

    // WM_NCCALCSIZE makes this a fully client-rendered title bar.  ImGui draws
    // its glyphs while DWM and HT* caption results retain native behavior.
    const Editor::CaptionButtonBounds captionButtons = titleBar.captionButtonBounds();
    if (captionButtons.valid()) {
        ImDrawList *const drawList = ImGui::GetWindowDrawList();
        const float buttonWidth = (captionButtons.right - captionButtons.left) / 3.0F;
        const float buttonHeight = captionButtons.bottom - captionButtons.top;
        const ImVec2 mouse = ImGui::GetMousePos();
        const float glyphHalfExtent = std::round(5.0F * titleBar.dpiScale());
        const bool activeWindow = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        const ImU32 glyphColor = activeWindow
                                     ? IM_COL32(230, 235, 245, 255)
                                     : IM_COL32(230, 235, 245, 155);

        for (int index = 0; index < 3; ++index) {
            const ImVec2 minimum{
                captionButtons.left + buttonWidth * static_cast<float>(index),
                captionButtons.top
            };
            const ImVec2 maximum{minimum.x + buttonWidth, captionButtons.bottom};
            const bool hovered = mouse.x >= minimum.x && mouse.x < maximum.x &&
                                 mouse.y >= minimum.y && mouse.y < maximum.y;
            const bool pressed = hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left);
            if (hovered) {
                const ImU32 hoverColor = index == 2
                                             ? IM_COL32(196, 43, 28, activeWindow ? (pressed ? 255 : 235) : 150)
                                             : IM_COL32(255, 255, 255, pressed ? 28 : 16);
                drawList->AddRectFilled(minimum, maximum, hoverColor);
            }

            const ImVec2 center{
                pixelAligned(minimum.x + buttonWidth * 0.5F),
                pixelAligned(minimum.y + buttonHeight * 0.5F)
            };
            if (index == 0) {
                const float lineY = pixelAligned(center.y + std::round(2.0F * titleBar.dpiScale()));
                drawList->AddLine({pixelAligned(center.x - glyphHalfExtent), lineY},
                                  {pixelAligned(center.x + glyphHalfExtent), lineY}, glyphColor, 1.0F);
            } else if (index == 1) {
                if (titleBar.isMaximized()) {
                    drawRestoreGlyph(drawList, center, glyphHalfExtent, glyphColor);
                } else {
                    drawList->AddRect({
                                          pixelAligned(center.x - glyphHalfExtent),
                                          pixelAligned(center.y - glyphHalfExtent)
                                      },
                                      {
                                          pixelAligned(center.x + glyphHalfExtent),
                                          pixelAligned(center.y + glyphHalfExtent)
                                      },
                                      glyphColor, 0.0F, 0, 1.0F);
                }
            } else {
                drawList->AddLine({
                                      pixelAligned(center.x - glyphHalfExtent),
                                      pixelAligned(center.y - glyphHalfExtent)
                                  },
                                  {
                                      pixelAligned(center.x + glyphHalfExtent),
                                      pixelAligned(center.y + glyphHalfExtent)
                                  }, glyphColor, 1.0F);
                drawList->AddLine({
                                      pixelAligned(center.x + glyphHalfExtent),
                                      pixelAligned(center.y - glyphHalfExtent)
                                  },
                                  {
                                      pixelAligned(center.x - glyphHalfExtent),
                                      pixelAligned(center.y + glyphHalfExtent)
                                  }, glyphColor, 1.0F);
            }
        }
    }
    ImGui::EndMainMenuBar();
    ImGui::PopStyleVar();

    if (openNewProject) {
        ImGui::OpenPopup("New Project");
        openNewProject = false;
    }
    if (ImGui::BeginPopupModal("New Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Create a portable GamEngine project.");
        ImGui::TextDisabled("The project manifest and starter scene are created automatically.");
        ImGui::Separator();
        ImGui::SetNextItemWidth(420.0F);
        ImGui::InputText("Name", newProjectName, sizeof(newProjectName));
        ImGui::SetNextItemWidth(420.0F);
        ImGui::InputTextWithHint("Location", "D:/Projects/MyGame", newProjectLocation,
                                 sizeof(newProjectLocation));
        ImGui::TextDisabled("Template");
        ImGui::SetNextItemWidth(420.0F);
        ImGui::BeginDisabled();
        char templateName[] = "3D Game";
        ImGui::InputText("##project-template", templateName, sizeof(templateName), ImGuiInputTextFlags_ReadOnly);
        ImGui::EndDisabled();
        ImGui::TextDisabled("Assets/Models, Materials, Textures, Scenes, Scripts and Audio will be created.");
        if (!newProjectError.empty()) ImGui::TextColored(EditorUI::colors().error, "%s", newProjectError.c_str());
        const bool valid = newProjectName[0] != '\0' && newProjectLocation[0] != '\0';
        ImGui::BeginDisabled(!valid);
        if (EditorButton("Create", {110.0F, 0.0F}).draw()) {
            createProject();
            if (newProjectError.empty()) ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (EditorButton("Cancel", {110.0F, 0.0F}).draw()) {
            newProjectError.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (openSceneSettings) {
        if (antialiasingType < 0) {
            antialiasingType = renderer.antialiasingLevel() == Engine::AntialiasingLevel::Off
                                   ? 0
                                   : renderer.antialiasingLevel() == Engine::AntialiasingLevel::TAA
                                         ? 3
                                         : 1;
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
