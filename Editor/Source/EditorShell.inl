struct StatusBarState final {
    std::size_t selectionCount{};
    bool playing{};
    bool paused{};
    bool unsavedChanges{};
    bool gameBuildRunning{};
};

void drawStatusBar(const Engine::ScenePreset& scene, const Engine::Entity selected, const StatusBarState& state) {
    const EditorUI::Palette& palette = EditorUI::colors();
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float barHeight = static_cast<float>(EditorConstants::statusBarHeight);
    ImGui::SetNextWindowPos({viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - barHeight});
    ImGui::SetNextWindowSize({viewport->WorkSize.x, barHeight});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, palette.titleBar);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10.0F, 0.0F});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::Begin("##status-bar", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 windowPos = ImGui::GetWindowPos();
    drawList->AddLine(windowPos, {windowPos.x + ImGui::GetWindowWidth(), windowPos.y},
                      ImGui::GetColorU32(palette.border));
    const float textY = std::round((barHeight - ImGui::GetTextLineHeight()) * 0.5F);
    const auto separator = [&] {
        ImGui::SameLine(0.0F, 12.0F);
        const ImVec2 position = ImGui::GetCursorScreenPos();
        drawList->AddLine({position.x, windowPos.y + barHeight * 0.28F}, {position.x, windowPos.y + barHeight * 0.72F},
                          ImGui::GetColorU32(palette.border));
        ImGui::Dummy({1.0F, 1.0F});
        ImGui::SameLine(0.0F, 12.0F);
        ImGui::SetCursorPosY(textY);
    };

    // Mode pill: Ready / Playing / Paused.
    const ImVec4 statusColor = state.playing ? (state.paused ? palette.warning : palette.success) : palette.accent;
    const char* statusText = state.playing ? (state.paused ? "Paused" : "Playing") : "Ready";
    ImGui::SetCursorPosY(textY - 1.0F);
    EditorUI::badge(statusText, EditorUI::withAlpha(statusColor, 0.16F), statusColor);

    separator();
    const std::filesystem::path scenePath = EditorSceneSession::scenePath();
    const std::string sceneName = scenePath.empty() ? std::string{"Untitled scene"} : scenePath.filename().string();
    ImGui::TextUnformatted(sceneName.c_str());
    if (state.unsavedChanges) {
        ImGui::SameLine(0.0F, 6.0F);
        ImGui::TextColored(palette.warning, "(unsaved)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Changes are auto-saved every 30 seconds, or press Ctrl+S.");
    }

    separator();
    ImGui::TextDisabled("%zu objects", scene.view().size());
    separator();
    if (selected == Engine::NullEntity || state.selectionCount == 0) {
        ImGui::TextDisabled("No selection");
    } else if (state.selectionCount > 1) {
        ImGui::TextDisabled("%zu selected", state.selectionCount);
    } else {
        ImGui::TextDisabled("Selected:");
        ImGui::SameLine(0.0F, 4.0F);
        ImGui::TextUnformatted(entityName(scene, selected));
    }

    // Right-aligned: background work and frame timing.
    const ImGuiIO& io = ImGui::GetIO();
    char timing[64]{};
    std::snprintf(timing, sizeof(timing), "%.0f FPS  %.2f ms", io.Framerate,
                  io.Framerate > 0.0F ? 1000.0F / io.Framerate : 0.0F);
    const float timingWidth = ImGui::CalcTextSize(timing).x;
    const char* buildText = "Building game...";
    const float buildWidth = state.gameBuildRunning ? ImGui::CalcTextSize(buildText).x + 40.0F : 0.0F;
    const float rightX = ImGui::GetWindowContentRegionMax().x - timingWidth - buildWidth;
    ImGui::SameLine();
    if (rightX > ImGui::GetCursorPosX() + 16.0F) {
        ImGui::SetCursorPosX(rightX);
        if (state.gameBuildRunning) {
            const ImVec2 position = ImGui::GetCursorScreenPos();
            const ImVec2 center{position.x + 7.0F, windowPos.y + barHeight * 0.5F};
            const float time = static_cast<float>(ImGui::GetTime());
            drawList->PathArcTo(center, 6.0F, time * 6.0F, time * 6.0F + 4.2F, 14);
            drawList->PathStroke(ImGui::GetColorU32(palette.accent), ImDrawFlags_None, 2.0F);
            ImGui::Dummy({18.0F, 1.0F});
            ImGui::SameLine(0.0F, 4.0F);
            ImGui::SetCursorPosY(textY);
            ImGui::TextColored(palette.accent, "%s", buildText);
            ImGui::SameLine(0.0F, 18.0F);
            ImGui::SetCursorPosY(textY);
        }
        ImGui::TextDisabled("%s", timing);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

[[maybe_unused]] void drawLegacyGpuProfilePanel(const Engine::Renderer& renderer, bool& isOpen) {
    (void)renderer;
    static bool writeCsv{};
    static float intervalSeconds{2.0F};
    static std::chrono::steady_clock::time_point lastWrite{};
    static std::string writeError;
    const std::filesystem::path csvPath = Platform::UserPaths::editorLogs() / "gpu-profile.csv";
    const std::uint32_t historyCount = Engine::Profiler::historySize();
    const Engine::ProfileFrame* latestGpuFrame = nullptr;
    for (std::uint32_t index = historyCount; index != 0; --index) {
        const Engine::ProfileFrame& frame = Engine::Profiler::historyFrame(index - 1);
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
            const bool writeHeader =
                !std::filesystem::exists(csvPath, error) || (!error && std::filesystem::file_size(csvPath, error) == 0);
            std::ofstream output{csvPath, std::ios::app};
            if (!output) {
                writeError = "Could not open GPU profile CSV for writing.";
            } else {
                if (writeHeader) {
                    output << "timestamp_unix_ms,gpu_frame_ms,profiled_zones_ms\n";
                }
                float profiledMilliseconds = 0.0F;
                for (const Engine::GpuProfileEvent& event : latestGpuFrame->gpuEvents) {
                    if (event.depth == 0)
                        profiledMilliseconds += event.endMs - event.startMs;
                }
                const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count();
                output << timestamp << ',' << latestGpuFrame->gpuFrameMs << ',' << profiledMilliseconds << '\n';
                writeError.clear();
                lastWrite = monotonicNow;
            }
        }
    }

    if (!isOpen)
        return;
    if (!ImGui::Begin("GPU Profile", &isOpen)) {
        ImGui::End();
        return;
    }

    static bool followLatest{true};
    static std::uint64_t selectedFrame{};
    if (ImGui::Button(followLatest ? "Lock frame" : "Follow latest"))
        followLatest = !followLatest;
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        Engine::Profiler::clear();
        selectedFrame = 0;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Write CSV", &writeCsv))
        lastWrite = {};
    ImGui::SameLine();
    ImGui::SetNextItemWidth(88.0F);
    if (ImGui::DragFloat("##gpu-profile-interval", &intervalSeconds, 0.25F, 0.25F, 60.0F, "every %.2f s")) {
        lastWrite = {};
    }
    if (writeCsv)
        ImGui::TextDisabled("%s", csvPath.string().c_str());
    if (!writeError.empty())
        ImGui::TextColored(EditorUI::colors().error, "%s", writeError.c_str());
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
            const Engine::ProfileFrame& frame = Engine::Profiler::historyFrame(index);
            cpuTimes[index] = static_cast<float>(frame.cpuFrameMs);
            gpuTimes[index] = static_cast<float>(frame.gpuFrameMs);
            maxCpu = std::max(maxCpu, cpuTimes[index]);
            maxGpu = std::max(maxGpu, gpuTimes[index]);
        }
        const float graphMax = std::max({16.667F, maxCpu, maxGpu});
        ImGui::TextUnformatted("Frame time history");
        ImGui::PlotLines("CPU (ms)", cpuTimes.data(), static_cast<int>(historyCount), 0, nullptr, 0.0F, graphMax,
                         {0.0F, 72.0F});
        ImGui::PlotLines("GPU (ms)", gpuTimes.data(), static_cast<int>(historyCount), 0, nullptr, 0.0F, graphMax,
                         {0.0F, 72.0F});
        if (ImGui::BeginCombo("Selected frame", ("#" + std::to_string(selectedFrame)).c_str())) {
            for (std::uint32_t index = 0; index < historyCount; ++index) {
                const Engine::ProfileFrame& frame = Engine::Profiler::historyFrame(index);
                const bool selected = frame.frameNumber == selectedFrame;
                const std::string label = "#" + std::to_string(frame.frameNumber) + "  CPU " +
                                          std::to_string(frame.cpuFrameMs).substr(0, 5) + " ms";
                if (ImGui::Selectable(label.c_str(), selected)) {
                    selectedFrame = frame.frameNumber;
                    followLatest = false;
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        const std::uint64_t targetFrame = selectedFrame;
        const Engine::ProfileFrame* selected = nullptr;
        for (std::uint32_t index = 0; index < historyCount; ++index) {
            const Engine::ProfileFrame& frame = Engine::Profiler::historyFrame(index);
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
                    const auto& event = selected->cpuEvents[index];
                    const double total = static_cast<double>(event.endNs - event.startNs) * 1.0e-6;
                    std::uint64_t childNs{};
                    for (std::size_t child = index + 1; child < selected->cpuEvents.size(); ++child) {
                        const auto& candidate = selected->cpuEvents[child];
                        if (candidate.depth <= event.depth)
                            break;
                        if (candidate.depth == event.depth + 1)
                            childNs += candidate.endNs - candidate.startNs;
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

    const Engine::ProfileFrame* selectedGpuFrame = nullptr;
    for (std::uint32_t index = 0; index < historyCount; ++index) {
        const Engine::ProfileFrame& frame = Engine::Profiler::historyFrame(index);
        if (frame.frameNumber == selectedFrame) {
            selectedGpuFrame = &frame;
            break;
        }
    }
    float profiledMilliseconds = 0.0F;
    if (selectedGpuFrame && selectedGpuFrame->gpuReady &&
        ImGui::BeginTable("##gpu-profile-passes", 2,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("GPU zone");
        ImGui::TableSetupColumn("GPU time", ImGuiTableColumnFlags_WidthFixed, 92.0F);
        ImGui::TableHeadersRow();
        for (const Engine::GpuProfileEvent& event : selectedGpuFrame->gpuEvents) {
            const float milliseconds = event.endMs - event.startMs;
            if (event.depth == 0)
                profiledMilliseconds += milliseconds;
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
bool beginTopMenu(const char* label, const char* tooltip, Editor::WindowsTitleBar& titleBar) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10.0F, ImGui::GetStyle().FramePadding.y});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {2.0F, 0.0F});
    ImGui::PushStyleColor(ImGuiCol_Header, EditorUI::colors().surfaceRaised);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, EditorUI::colors().controlHover);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, EditorUI::colors().accentMuted);

    const bool open = ImGui::BeginMenu(label);
    const ImVec2 itemMin = ImGui::GetItemRectMin();
    const ImVec2 itemMax = ImGui::GetItemRectMax();
    titleBar.addHitRegion(itemMin.x, itemMin.y, itemMax.x, itemMax.y, Editor::WindowsTitleBar::clientHitTestResult);
    if (open) {
        // Accent underline marks the open menu in the bar.
        ImGui::GetForegroundDrawList(ImGui::GetMainViewport())
            ->AddLine({itemMin.x + 6.0F, itemMax.y - 1.5F}, {itemMax.x - 6.0F, itemMax.y - 1.5F},
                      ImGui::GetColorU32(EditorUI::colors().accent), 2.0F);
        // The bar uses tall frames and tight spacing; menu contents need
        // regular controls and comfortable rows instead.
        ImGui::PopStyleVar(2);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8.0F, 4.0F});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {10.0F, 7.0F});
        return true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && tooltip != nullptr) {
        ImGui::SetTooltip("%s", tooltip);
    }

    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
    return false;
}

void endTopMenu() {
    ImGui::EndMenu();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
}

// Every menu row reserves the same icon column so labels line up whether or
// not an entry has an icon. The column is reserved with spaces and the vector
// icon is drawn into it afterwards.
constexpr const char* kMenuIconSlot = "    ";

void drawMenuIcon(const ImVec2 rowStart, const std::optional<EditorUI::Icon> icon, const bool enabled,
                  const ImVec4& color) {
    if (!icon)
        return;
    const float offset = ImGui::GetCurrentWindow()->DC.MenuColumns.OffsetIcon;
    const float size = ImGui::GetFontSize() * 0.86F;
    ImVec4 tint = color;
    if (!enabled)
        tint.w *= ImGui::GetStyle().DisabledAlpha;
    EditorUI::drawIcon(
        ImGui::GetWindowDrawList(), *icon,
        {std::round(rowStart.x + offset + size * 0.5F), std::round(rowStart.y + ImGui::GetTextLineHeight() * 0.5F)},
        size, ImGui::GetColorU32(tint));
}

bool menuItem(const char* label, const char* shortcut = nullptr,
              const std::optional<EditorUI::Icon> icon = std::nullopt, const bool selected = false,
              const bool enabled = true) {
    const ImVec2 rowStart = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::MenuItemEx(label, kMenuIconSlot, shortcut, selected, enabled);
    // An enclosing BeginDisabled() is already applied through style alpha.
    drawMenuIcon(rowStart, icon, enabled, EditorUI::colors().textSecondary);
    return pressed;
}

/** Checkable entry that toggles @p value, e.g. a panel visibility switch. */
bool menuToggle(const char* label, const std::optional<EditorUI::Icon> icon, bool& value) {
    if (!menuItem(label, nullptr, icon, value))
        return false;
    value = !value;
    return true;
}

bool beginSubMenu(const char* label, const std::optional<EditorUI::Icon> icon = std::nullopt,
                  const bool enabled = true) {
    const ImVec2 rowStart = ImGui::GetCursorScreenPos();
    const bool open = ImGui::BeginMenuEx(label, kMenuIconSlot, enabled);
    // BeginMenuEx switches to the child menu window when it opens; draw into
    // the parent row using its own draw list.
    if (icon) {
        ImGuiWindow* parent = open ? ImGui::GetCurrentWindow()->ParentWindow : ImGui::GetCurrentWindow();
        if (parent != nullptr) {
            const float size = ImGui::GetFontSize() * 0.86F;
            ImVec4 tint = EditorUI::colors().textSecondary;
            if (!enabled)
                tint.w *= ImGui::GetStyle().DisabledAlpha;
            EditorUI::drawIcon(parent->DrawList, *icon,
                               {std::round(rowStart.x + parent->DC.MenuColumns.OffsetIcon + size * 0.5F),
                                std::round(rowStart.y + ImGui::GetTextLineHeight() * 0.5F)},
                               size, ImGui::GetColorU32(tint));
        }
    }
    return open;
}

/** Small caption that groups the entries below it inside a menu. */
void menuSection(const char* title) {
    ImGui::PushStyleColor(ImGuiCol_Text, EditorUI::colors().textSecondary);
    ImGui::SeparatorText(title);
    ImGui::PopStyleColor();
}

[[nodiscard]] float pixelAligned(const float value) {
    return std::round(value) + 0.5F;
}

void drawRestoreGlyph(ImDrawList* const drawList, const ImVec2 center, const float glyphHalfExtent,

                      const ImU32 color) {
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

Engine::Entity
drawEditorMenuBar(Engine::ScenePreset& scene, Engine::Renderer& renderer, Engine::Assets::Content& content,
                  Engine::Project& project, const std::filesystem::path& editorRoot, Editor::WindowsTitleBar& titleBar,
                  bool& antialiasingChanged, bool& sceneLoaded, bool& sceneSaved, bool& sceneDeleted,
                  const bool playing, const bool paused, bool& playToggleRequested, bool& pauseToggleRequested,
                  bool& restartRequested, const bool canUndo, const bool canRedo, const bool canPaste,
                  bool& undoRequested, bool& redoRequested, bool& copyRequested, bool& pasteRequested,
                  bool& duplicateRequested, bool& resetHistoryRequested, bool& showHierarchy, bool& showViewport,
                  bool& showInspector, bool& showAssetManager, bool& showTerrainTools, bool& showConsole,
                  bool& showTerminal, bool& showShaderGraph, bool& showGpuProfile, bool& gameBuildRequested) {
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

    const auto saveScene = [&](const std::filesystem::path& path) {
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
        EditorSceneSession::requestFileDialog(EditorSceneSession::FileDialogAction::SaveScene);
    };
    const auto loadScene = [&](const std::filesystem::path& path) {
        std::optional<Engine::AntialiasingLevel> antialiasing;
        // Loading may fail after replacing the registry (for example while
        // reading environment settings). The caller must still reload it.
        sceneLoaded = true;
        Engine::SceneSerializer::load(scene, path, antialiasing);
        resetHistoryRequested = true;
        if (!scene.environmentEquirectangular().empty() &&
            !renderer.setEnvironmentEquirectangular(content.assetRoot() / scene.environmentEquirectangular())) {
            throw std::runtime_error("Could not load scene HDR/EXR environment");
        }
        EditorSceneSession::markSceneSaved(path);
        if (antialiasing) {
            renderer.setAntialiasingLevel(*antialiasing);
            antialiasingChanged = true;
        }
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
    const auto createSceneFromPath = [&](const std::filesystem::path& path) {
        try {
            if (!path.parent_path().empty()) {
                std::filesystem::create_directories(path.parent_path());
            }
            Engine::ScenePreset emptyScene;
            Engine::SceneSerializer::save(emptyScene, path, EditorSceneSession::antialiasingLevel(renderer));
            sceneLoaded = true;
            Engine::SceneSerializer::load(scene, path);
            resetHistoryRequested = true;
            EditorSceneSession::markSceneSaved(path);
            sceneSaved = true;
            sceneFileError.clear();
            Editor::ConsolePanel::info("Created scene: " + path.string());
        } catch (const std::exception& error) {
            sceneFileError = error.what();
            Editor::ConsolePanel::error("Could not create scene: " + sceneFileError);
        }
    };
    const auto createScene = [&] {
        EditorSceneSession::requestFileDialog(EditorSceneSession::FileDialogAction::NewScene);
    };
    const auto createProject = [&] {
        try {
            const std::filesystem::path root{newProjectLocation};
            Engine::Project createdProject = Engine::Project::create(root, newProjectName);
            Editor::ensureProjectBrdfLut(createdProject.assetRoot(), editorRoot);
            content.clear();
            content.setAssetRoot(createdProject.assetRoot());
            project = std::move(createdProject);
            EditorSceneSession::setProjectRoot(project.rootPath(), project.assetRoot());
            loadScene(project.startupScene());
            newProjectError.clear();
            Editor::ConsolePanel::info("Created project: " + project.name());
        } catch (const std::exception& error) {
            newProjectError = error.what();
            Editor::ConsolePanel::error("Could not create project: " + newProjectError);
        }
    };

    const auto loadProject = [&](const std::filesystem::path& manifestPath) {
        try {
            Engine::Project loadedProject = Engine::Project::load(manifestPath);
            Editor::ensureProjectBrdfLut(loadedProject.assetRoot(), editorRoot);
            content.clear();
            content.setAssetRoot(loadedProject.assetRoot());
            project = std::move(loadedProject);
            EditorSceneSession::setProjectRoot(project.rootPath(), project.assetRoot());
            if (std::filesystem::is_regular_file(project.startupScene())) {
                loadScene(project.startupScene());
            } else {
                EditorSceneSession::clearSavedScene();
                sceneDeleted = true;
            }
            Editor::ConsolePanel::info("Loaded project: " + project.name());
        } catch (const std::exception& error) {
            sceneFileError = error.what();
            Editor::ConsolePanel::error("Could not load scene: " + sceneFileError);
        }
    };
    if (const auto result = EditorSceneSession::pollFileDialog()) {
        if (!result->error.empty()) {
            sceneFileError = result->error;
            Editor::ConsolePanel::error("File dialog failed: " + sceneFileError);
        } else if (result->path && result->projectRoot == project.rootPath() && !playing) {
            try {
                switch (result->action) {
                case EditorSceneSession::FileDialogAction::SaveScene:
                    saveScene(*result->path);
                    break;
                case EditorSceneSession::FileDialogAction::NewScene:
                    createSceneFromPath(*result->path);
                    break;
                case EditorSceneSession::FileDialogAction::LoadScene:
                    tryLoadScene(*result->path);
                    break;
                case EditorSceneSession::FileDialogAction::LoadProject:
                    loadProject(*result->path);
                    break;
                }
            } catch (const std::exception& error) {
                sceneFileError = error.what();
                Editor::ConsolePanel::error("Could not complete file operation: " + sceneFileError);
            }
        }
    }

    titleBar.clearHitRegions();
    const float fontHeight = ImGui::GetFontSize();
    // Without a native caption (Linux) give the bar the same breathing room
    // as the Windows title bar, so menus and play controls are not cramped.
    const float titleHeight = std::max(static_cast<float>(titleBar.nativeHeight()), std::round(fontHeight + 14.0F));
    const float paddingY = std::max(0.0F, (titleHeight - fontHeight) * 0.5F);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8.0F, paddingY});
    if (!ImGui::BeginMainMenuBar()) {
        ImGui::PopStyleVar();
        return Engine::NullEntity;
    }
    const float captionButtonsStart = titleBar.contentRight();
    if (captionButtonsStart > 0.0F) {
        const ImVec2 menuBarOrigin = ImGui::GetWindowPos();
        ImGui::PushClipRect(menuBarOrigin, {captionButtonsStart - 8.0F, menuBarOrigin.y + ImGui::GetFrameHeight()},
                            true);
    }

    const std::filesystem::path activeScenePath = EditorSceneSession::scenePath();
    const EditorUI::Palette& palette = EditorUI::colors();
    const auto saveCurrentScene = [&] {
        try {
            if (EditorSceneSession::hasSavedScene()) {
                saveScene(activeScenePath);
            } else {
                saveSceneAs();
            }
        } catch (const std::exception& error) {
            sceneFileError = error.what();
            Editor::ConsolePanel::error("Could not save scene: " + sceneFileError);
        }
    };
    const auto requestNewProject = [&] {
        if (newProjectLocation[0] == '\0') {
            const std::string defaultLocation = (std::filesystem::current_path() / newProjectName).string();
            std::snprintf(newProjectLocation, sizeof(newProjectLocation), "%s", defaultLocation.c_str());
        }
        newProjectError.clear();
        openNewProject = true;
    };

    // File shortcuts shown in the menu. Global routing lets a focused panel
    // with its own binding (Shader Graph's Ctrl+S) take precedence.
    if (!playing) {
        constexpr ImGuiInputFlags route = ImGuiInputFlags_RouteGlobal;
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, route))
            saveCurrentScene();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, route))
            saveSceneAs();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_N, route))
            createScene();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, route))
            EditorSceneSession::requestFileDialog(EditorSceneSession::FileDialogAction::LoadProject);
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_O, route))
            EditorSceneSession::requestFileDialog(EditorSceneSession::FileDialogAction::LoadScene);
    }

    // Engine mark on the far left.
    {
        const float barHeight = ImGui::GetFrameHeight();
        const float markSize = std::round(barHeight * 0.62F);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4.0F);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::Dummy({markSize + 6.0F, barHeight});
        const ImVec2 center{std::round(origin.x + markSize * 0.5F), std::round(origin.y + barHeight * 0.5F)};
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled({center.x - markSize * 0.5F, center.y - markSize * 0.5F},
                                {center.x + markSize * 0.5F, center.y + markSize * 0.5F},
                                ImGui::GetColorU32(palette.accentMuted), 5.0F);
        EditorUI::drawIcon(drawList, EditorUI::Icon::Mesh, center, markSize * 0.72F,
                           ImGui::GetColorU32(palette.accentHover));
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("GamEngine Editor");
        ImGui::SameLine(0.0F, 4.0F);
    }

    if (beginTopMenu("File", "Projects, scenes and game builds", titleBar)) {
        ImGui::BeginDisabled(playing);
        menuSection("Scene");
        if (menuItem("New Scene...", "Ctrl+N", EditorUI::Icon::File))
            createScene();
        if (menuItem("Open Scene...", "Ctrl+Alt+O", EditorUI::Icon::Folder))
            EditorSceneSession::requestFileDialog(EditorSceneSession::FileDialogAction::LoadScene);
        if (beginSubMenu("Project Scenes", EditorUI::Icon::Layers)) {
            const auto scenes = project.scenes();
            if (scenes.empty()) {
                ImGui::TextDisabled("No scenes in %s", (project.assetRoot() / "Scenes").string().c_str());
            }
            for (const auto& path : scenes) {
                const std::string label = path.lexically_relative(project.rootPath()).string();
                const bool active = path == activeScenePath;
                if (menuItem(label.c_str(), nullptr, std::nullopt, active, !active)) {
                    tryLoadScene(path);
                }
            }
            ImGui::EndMenu();
        }
        if (menuItem("Save Scene", "Ctrl+S", EditorUI::Icon::Save))
            saveCurrentScene();
        if (menuItem("Save Scene As...", "Ctrl+Shift+S"))
            saveSceneAs();
        if (menuItem("Delete Scene...", nullptr, EditorUI::Icon::Trash, false, EditorSceneSession::hasSavedScene()))
            openDeleteScene = true;
        menuSection("Project");
        if (menuItem("New Project...", nullptr, EditorUI::Icon::Plus))
            requestNewProject();
        if (menuItem("Open Project...", "Ctrl+O", EditorUI::Icon::Folder))
            EditorSceneSession::requestFileDialog(EditorSceneSession::FileDialogAction::LoadProject);
        if (menuItem("Build Game", nullptr, EditorUI::Icon::Archive, false, !project.manifestPath().empty()))
            gameBuildRequested = true;
        ImGui::EndDisabled();
        if (!sceneFileError.empty()) {
            ImGui::Separator();
            EditorUI::iconLabel(EditorUI::Icon::Error, palette.error);
            ImGui::SameLine(0.0F, 6.0F);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 320.0F);
            ImGui::TextColored(palette.error, "%s", sceneFileError.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::Separator();
        ImGui::TextDisabled("Scenes auto-save every 30 seconds after changes.");
        endTopMenu();
    }

    if (openDeleteScene)
        ImGui::OpenPopup("##confirm-delete-scene");
    if (ImGui::BeginPopupModal("##confirm-delete-scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        EditorUI::iconLabel(EditorUI::Icon::Warning, EditorUI::colors().warning);
        ImGui::SameLine(0.0F, 6.0F);
        ImGui::TextWrapped("Delete scene '%s'?", activeScenePath.filename().string().c_str());
        ImGui::TextDisabled("This permanently removes the scene file from the project.");
        ImGui::Separator();
        if (EditorUI::dangerButton("Delete", {120.0F, 0.0F})) {
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

    if (beginTopMenu("Edit", "Undo, clipboard and Play Mode", titleBar)) {
        if (menuItem("Undo", "Ctrl+Z", EditorUI::Icon::Back, false, canUndo && !playing))
            undoRequested = true;
        if (menuItem("Redo", "Ctrl+Y", std::nullopt, false, canRedo && !playing))
            redoRequested = true;
        ImGui::Separator();
        if (menuItem("Copy", "Ctrl+C", std::nullopt, false, !playing))
            copyRequested = true;
        if (menuItem("Paste", "Ctrl+V", std::nullopt, false, canPaste && !playing))
            pasteRequested = true;
        if (menuItem("Duplicate", "Ctrl+D", std::nullopt, false, !playing))
            duplicateRequested = true;
        menuSection("Play Mode");
        if (menuItem(playing ? "Stop" : "Play", "F5", playing ? EditorUI::Icon::Stop : EditorUI::Icon::Play))
            playToggleRequested = true;
        if (menuItem(paused ? "Resume" : "Pause", "F6", EditorUI::Icon::Pause, paused, playing))
            pauseToggleRequested = true;
        if (menuItem("Restart", nullptr, EditorUI::Icon::Restart, false, playing))
            restartRequested = true;
        endTopMenu();
    }

    if (beginTopMenu("View", "Show, hide and arrange editor panels", titleBar)) {
        menuSection("Panels");
        menuToggle("Hierarchy", EditorUI::Icon::List, showHierarchy);
        menuToggle("Viewport", EditorUI::Icon::Frame, showViewport);
        menuToggle("Inspector", EditorUI::Icon::Sidebar, showInspector);
        menuToggle("Asset Manager", EditorUI::Icon::Folder, showAssetManager);
        menuToggle("Terrain Tools", EditorUI::Icon::Terrain, showTerrainTools);
        menuToggle("Console", EditorUI::Icon::Info, showConsole);
        menuToggle("Terminal", EditorUI::Icon::Code, showTerminal);
        menuToggle("Shader Graph", EditorUI::Icon::Layers, showShaderGraph);
        menuToggle("Profiler", EditorUI::Icon::Gear, showGpuProfile);
        ImGui::Separator();
        if (menuItem("Reset Layout", nullptr, EditorUI::Icon::Refresh)) {
            showHierarchy = showViewport = showInspector = showAssetManager = true;
            showTerrainTools = showConsole = showTerminal = true;
            EditorStyle::requestDockLayoutReset();
        }
        endTopMenu();
    }

    if (beginTopMenu("GameObject", "Create objects in the current scene", titleBar)) {
        ImGui::BeginDisabled(playing);
        if (menuItem("Create Empty", "Ctrl+Shift+N", EditorUI::Icon::GameObject))
            createdEntity = scene.createGameObject();
        if (beginSubMenu("3D Object", EditorUI::Icon::Mesh)) {
            if (menuItem("Cube"))
                createdEntity = scene.createCube();
            if (menuItem("Sphere"))
                createdEntity = scene.createSphere();
            if (menuItem("Capsule"))
                createdEntity = scene.createCapsule();
            if (menuItem("Plane"))
                createdEntity = scene.createPlane();
            if (menuItem("Ramp"))
                createdEntity = scene.createRamp();
            ImGui::Separator();
            if (menuItem("Glass"))
                createdEntity = scene.createGlass();
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (menuItem("Camera", nullptr, EditorUI::Icon::Camera))
            createdEntity = scene.createCamera();
        if (menuItem("Light", nullptr, EditorUI::Icon::Light))
            createdEntity = scene.createLight();
        ImGui::Separator();
        if (menuItem("Terrain", nullptr, EditorUI::Icon::Terrain))
            createdEntity = scene.createTerrain();
        if (beginSubMenu("Water", EditorUI::Icon::Water)) {
            if (menuItem("Ocean"))
                createdEntity = scene.createOcean();
            if (menuItem("Lake"))
                createdEntity = scene.createLake();
            if (menuItem("River"))
                createdEntity = scene.createRiver();
            ImGui::EndMenu();
        }
        if (menuItem("Procedural Cloud", nullptr, EditorUI::Icon::Cloud))
            createdEntity = scene.createProceduralCloud();
        ImGui::EndDisabled();
        endTopMenu();
    }

    if (beginTopMenu("Rendering", "Renderer quality, features and debug views", titleBar)) {
        menuSection("Quality preset");
        constexpr std::array<const char*, 4> presetLabels{"Low", "Medium", "High", "Ultra"};
        const auto currentPreset = [&] {
            for (const auto preset : {Engine::RenderQualityPreset::Low, Engine::RenderQualityPreset::Medium,
                                      Engine::RenderQualityPreset::High, Engine::RenderQualityPreset::Ultra}) {
                const auto settings = Engine::renderQualityPresetSettings(preset);
                if (renderer.shadowQuality() == settings.shadows && renderer.gtaoQuality() == settings.gtao &&
                    renderer.antialiasingLevel() == settings.antialiasing)
                    return preset;
            }
            return Engine::RenderQualityPreset::High;
        }();
        const int currentIndex = static_cast<int>(currentPreset);
        for (int index = 0; index < static_cast<int>(presetLabels.size()); ++index) {
            if (menuItem(presetLabels[static_cast<std::size_t>(index)], nullptr, std::nullopt, currentIndex == index)) {
                renderer.applyRenderQualityPreset(static_cast<Engine::RenderQualityPreset>(index));
                antialiasingChanged = true;
                Editor::ConsolePanel::info(std::string{"Renderer preset: "} +
                                           presetLabels[static_cast<std::size_t>(index)]);
            }
        }
        menuSection("Features");
        if (menuItem("Antialiasing...", nullptr, EditorUI::Icon::Gear))
            openSceneSettings = true;
        if (beginSubMenu("Render Path", EditorUI::Icon::Layers)) {
            const Engine::RenderPath currentPath = renderer.renderPath();
            if (menuItem("Async compute", nullptr, std::nullopt, currentPath == Engine::RenderPath::AsyncCompute))
                renderer.setRenderPath(Engine::RenderPath::AsyncCompute);
            if (menuItem("Sequential", nullptr, std::nullopt, currentPath == Engine::RenderPath::Sequential))
                renderer.setRenderPath(Engine::RenderPath::Sequential);
            ImGui::EndMenu();
        }
        bool contactShadowsEnabled = renderer.contactShadowMode() == Engine::ContactShadowMode::RayTraced;
        if (menuToggle("RT Contact Shadows (VSM)", EditorUI::Icon::Light, contactShadowsEnabled)) {
            renderer.setContactShadowMode(contactShadowsEnabled ? Engine::ContactShadowMode::RayTraced
                                                                : Engine::ContactShadowMode::Off);
        }
        if (contactShadowsEnabled) {
            ImGui::Indent(ImGui::GetFontSize() * 1.6F);
            EditorUI::badge(
                renderer.contactShadowsActive() ? "Active in Game View" : "Inactive in this view",
                EditorUI::withAlpha(renderer.contactShadowsActive() ? palette.success : palette.warning, 0.16F),
                renderer.contactShadowsActive() ? palette.success : palette.warning);
            ImGui::TextDisabled("Requires Game View, VSM shadows, Ray Query and no MSAA.");
            auto settings = renderer.rtContactShadowSettings();
            bool settingsChanged = false;
            ImGui::PushItemWidth(180.0F);
            settingsChanged |= ImGui::SliderFloat("Distance", &settings.maxDistance, 0.05F, 2.0F, "%.2f m");
            settingsChanged |= ImGui::SliderFloat("Normal bias", &settings.normalBias, 0.0005F, 0.05F, "%.4f");
            static float resolutionScaleDraft = -1.0F;
            if (resolutionScaleDraft < 0.0F)
                resolutionScaleDraft = settings.resolutionScale;
            ImGui::SliderFloat("Resolution", &resolutionScaleDraft, 0.25F, 1.0F, "%.2fx");
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                settings.resolutionScale = resolutionScaleDraft;
                settingsChanged = true;
            } else if (!ImGui::IsItemActive()) {
                resolutionScaleDraft = settings.resolutionScale;
            }
            ImGui::PopItemWidth();
            if (settingsChanged)
                renderer.setRtContactShadowSettings(settings);
            ImGui::Unindent(ImGui::GetFontSize() * 1.6F);
        }
        menuSection("Debug views");
        const Engine::ShadowDebugView shadowView = renderer.shadowDebugView();
        if (beginSubMenu(shadowView != Engine::ShadowDebugView::Off ? "Shadows (active)###shadow-debug"
                                                                    : "Shadows###shadow-debug",
                         EditorUI::Icon::Eye)) {
            const auto selectDebugView = [&](const char* label, const Engine::ShadowDebugView view) {
                if (menuItem(label, nullptr, std::nullopt, shadowView == view))
                    renderer.setShadowDebugView(view);
            };
            selectDebugView("Off", Engine::ShadowDebugView::Off);
            ImGui::Separator();
            selectDebugView("Blocker Count", Engine::ShadowDebugView::BlockerCount);
            selectDebugView("Penumbra Radius", Engine::ShadowDebugView::PenumbraRadius);
            selectDebugView("Desired Clip Level", Engine::ShadowDebugView::DesiredClipLevel);
            selectDebugView("Resolved Clip Level", Engine::ShadowDebugView::ResolvedClipLevel);
            selectDebugView("Fallback Delta", Engine::ShadowDebugView::FallbackDelta);
            selectDebugView("Virtual Page", Engine::ShadowDebugView::VirtualPage);
            selectDebugView("Residency", Engine::ShadowDebugView::Residency);
            ImGui::EndMenu();
        }
        const Engine::GtaoDebugView gtaoView = renderer.gtaoDebugView();
        if (beginSubMenu(gtaoView != Engine::GtaoDebugView::Off ? "Ambient Occlusion (active)###gtao-debug"
                                                                : "Ambient Occlusion###gtao-debug",
                         EditorUI::Icon::Eye)) {
            const auto selectGtaoDebugView = [&](const char* label, const Engine::GtaoDebugView view) {
                if (menuItem(label, nullptr, std::nullopt, gtaoView == view))
                    renderer.setGtaoDebugView(view);
            };
            selectGtaoDebugView("Final Lighting", Engine::GtaoDebugView::Off);
            ImGui::Separator();
            selectGtaoDebugView("GTAO Raw", Engine::GtaoDebugView::Raw);
            selectGtaoDebugView("GTAO Filtered", Engine::GtaoDebugView::Filtered);
            selectGtaoDebugView("GTAO Full", Engine::GtaoDebugView::Full);
            ImGui::EndMenu();
        }
        const Engine::PbrDebugView pbrView = renderer.pbrDebugView();
        if (beginSubMenu(pbrView != Engine::PbrDebugView::FinalLighting ? "PBR (active)###pbr-debug"
                                                                        : "PBR###pbr-debug",
                         EditorUI::Icon::Eye)) {
            const auto selectPbrDebugView = [&](const char* label, const Engine::PbrDebugView view) {
                if (menuItem(label, nullptr, std::nullopt, pbrView == view))
                    renderer.setPbrDebugView(view);
            };
            selectPbrDebugView("Final Lighting", Engine::PbrDebugView::FinalLighting);
            ImGui::Separator();
            selectPbrDebugView("Disable Specular IBL", Engine::PbrDebugView::NoSpecularIbl);
            selectPbrDebugView("Disable Direct Specular", Engine::PbrDebugView::NoDirectSpecular);
            selectPbrDebugView("Disable GTAO", Engine::PbrDebugView::NoGtao);
            ImGui::Separator();
            selectPbrDebugView("Geometric Direct Diffuse", Engine::PbrDebugView::GeometricDirectDiffuse);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        ImGui::TextDisabled("Shadows apply next frame; GTAO resources are rebuilt safely.");
        endTopMenu();
    }

    if (beginTopMenu("Help", "Shortcuts and editor information", titleBar)) {
        if (menuItem("Keyboard Shortcuts", nullptr, EditorUI::Icon::Info))
            showShortcuts = true;
        ImGui::Separator();
        if (menuItem("About GamEngine Editor"))
            showAbout = true;
        endTopMenu();
    }

    // Any active debug view changes what the viewport shows; flag it in the bar.
    const bool debugViewActive = renderer.shadowDebugView() != Engine::ShadowDebugView::Off ||
                                 renderer.gtaoDebugView() != Engine::GtaoDebugView::Off ||
                                 renderer.pbrDebugView() != Engine::PbrDebugView::FinalLighting;
    if (debugViewActive) {
        ImGui::SameLine(0.0F, 8.0F);
        ImGui::SetCursorPosY(std::round((ImGui::GetFrameHeight() - ImGui::GetTextLineHeight() - 2.0F) * 0.5F));
        EditorUI::badge("Debug view", EditorUI::withAlpha(palette.warning, 0.16F), palette.warning);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A Rendering > Debug view is active. Viewport shading is not final.");
        ImGui::SameLine();
    }
    const float menusEndX = ImGui::GetCursorPosX();

    // Play controls are centred in the title bar, where they stay reachable
    // whatever the dock layout is, instead of living inside one panel.
    float playGroupEndX = menusEndX;
    {
        const float barHeight = ImGui::GetFrameHeight();
        const float buttonHeight = std::round(barHeight * 0.76F);
        const ImVec2 buttonSize{std::round(buttonHeight * 1.35F), buttonHeight};
        constexpr float gap = 2.0F;
        const float groupWidth = buttonSize.x * 3.0F + gap * 2.0F;
        const float windowWidth = ImGui::GetWindowWidth();
        const float rightLimit =
            captionButtonsStart > 0.0F ? captionButtonsStart - ImGui::GetWindowPos().x - 8.0F : windowWidth - 8.0F;
        const float groupX = std::max(std::round((windowWidth - groupWidth) * 0.5F), menusEndX + 16.0F);
        if (groupX + groupWidth <= rightLimit) {
            playGroupEndX = groupX + groupWidth + 3.0F;
            const float groupY = std::round((barHeight - buttonHeight) * 0.5F);
            const ImVec2 groupMin{ImGui::GetWindowPos().x + groupX - 3.0F, ImGui::GetWindowPos().y + groupY - 3.0F};
            ImGui::GetWindowDrawList()->AddRectFilled(
                groupMin, {groupMin.x + groupWidth + 6.0F, groupMin.y + buttonHeight + 6.0F},
                ImGui::GetColorU32(playing ? EditorUI::withAlpha(palette.success, 0.12F) : palette.surface),
                ImGui::GetStyle().FrameRounding + 2.0F);
            const auto registerHitRegion = [&] {
                const ImVec2 min = ImGui::GetItemRectMin();
                const ImVec2 max = ImGui::GetItemRectMax();
                titleBar.addHitRegion(min.x, min.y, max.x, max.y, Editor::WindowsTitleBar::clientHitTestResult);
            };
            ImGui::SetCursorPos({groupX, groupY});
            if (EditorUI::iconButton(
                    "##title-play", playing ? EditorUI::Icon::Stop : EditorUI::Icon::Play,
                    playing ? "Stop Play Mode and restore the editor scene (F5)" : "Enter Play Mode (F5)",
                    {.active = playing, .size = buttonSize, .tint = playing ? palette.error : palette.success})) {
                playToggleRequested = true;
            }
            registerHitRegion();
            ImGui::SameLine(0.0F, gap);
            ImGui::SetCursorPosY(groupY);
            ImGui::BeginDisabled(!playing);
            if (EditorUI::iconButton("##title-pause", EditorUI::Icon::Pause, paused ? "Resume (F6)" : "Pause (F6)",
                                     {.active = paused,
                                      .size = buttonSize,
                                      .tint = paused ? std::optional{palette.warning} : std::nullopt})) {
                pauseToggleRequested = true;
            }
            registerHitRegion();
            ImGui::SameLine(0.0F, gap);
            ImGui::SetCursorPosY(groupY);
            if (EditorUI::iconButton("##title-restart", EditorUI::Icon::Restart,
                                     "Restart Play Mode from the editor scene", {.size = buttonSize})) {
                restartRequested = true;
            }
            registerHitRegion();
            ImGui::EndDisabled();
        }
    }

    // Right side: project name and the scene switcher. Scene navigation lives
    // in the bar so switching levels never needs a nested File menu.
    {
        const float barHeight = ImGui::GetFrameHeight();
        const float rightLimit = captionButtonsStart > 0.0F ? captionButtonsStart - ImGui::GetWindowPos().x - 12.0F
                                                            : ImGui::GetWindowWidth() - 10.0F;
        const std::string projectLabel = project.name();
        const float projectWidth = ImGui::CalcTextSize(projectLabel.c_str()).x + ImGui::GetFontSize() + 10.0F;
        const float chevronWidth = ImGui::GetFontSize() + 2.0F;
        float comboWidth = 210.0F;
        float startX = rightLimit - comboWidth - chevronWidth - projectWidth;
        bool showProject = true;
        if (startX < playGroupEndX + 16.0F) {
            showProject = false;
            comboWidth = std::max(120.0F, rightLimit - playGroupEndX - 16.0F);
            startX = rightLimit - comboWidth;
        }
        if (startX >= playGroupEndX + 16.0F) {
            const float textY = std::round((barHeight - ImGui::GetTextLineHeight()) * 0.5F);
            ImGui::SetCursorPos({startX, textY});
            if (showProject) {
                EditorUI::iconLabel(EditorUI::Icon::Folder, palette.warning);
                ImGui::SameLine(0.0F, 6.0F);
                ImGui::SetCursorPosY(textY);
                ImGui::TextDisabled("%s", projectLabel.c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", project.rootPath().string().c_str());
                ImGui::SameLine(0.0F, 2.0F);
                ImGui::SetCursorPosY(textY);
                EditorUI::iconLabel(EditorUI::Icon::ChevronRight, palette.textSecondary);
                ImGui::SameLine(0.0F, 2.0F);
            }
            const float comboHeight = std::round(barHeight * 0.76F);
            ImGui::SetCursorPosY(std::round((barHeight - comboHeight) * 0.5F));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                {8.0F, std::max(0.0F, (comboHeight - ImGui::GetFontSize()) * 0.5F)});
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {10.0F, 7.0F});
            ImGui::PushStyleColor(ImGuiCol_FrameBg, palette.surface);
            ImGui::BeginDisabled(playing);
            ImGui::SetNextItemWidth(comboWidth);
            const std::string activeSceneName =
                activeScenePath.empty() ? "No scene selected" : activeScenePath.filename().string();
            if (ImGui::BeginCombo("##active-project-scene", activeSceneName.c_str())) {
                if (ImGui::Selectable("No scene selected", activeScenePath.empty())) {
                    EditorSceneSession::clearSavedScene();
                    sceneDeleted = true;
                }
                ImGui::Separator();
                for (const auto& path : project.scenes()) {
                    const std::string label = path.lexically_relative(project.rootPath()).string();
                    const bool active = path == activeScenePath;
                    if (ImGui::Selectable(label.c_str(), active) && !active) {
                        tryLoadScene(path);
                    }
                    if (active)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::Separator();
                if (ImGui::Selectable("New Scene..."))
                    createScene();
                if (ImGui::Selectable("Open Scene..."))
                    EditorSceneSession::requestFileDialog(EditorSceneSession::FileDialogAction::LoadScene);
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
            const ImVec2 sceneSelectorMin = ImGui::GetItemRectMin();
            const ImVec2 sceneSelectorMax = ImGui::GetItemRectMax();
            titleBar.addHitRegion(sceneSelectorMin.x, sceneSelectorMin.y, sceneSelectorMax.x, sceneSelectorMax.y,
                                  Editor::WindowsTitleBar::clientHitTestResult);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s",
                                  playing ? "Stop Play Mode to switch scenes" : "Switch scenes in the current project");
        }
    }

    // Separate the bar from the dock space below.
    {
        const ImVec2 barMin = ImGui::GetWindowPos();
        const float barBottom = barMin.y + ImGui::GetWindowHeight() - 1.0F;
        ImGui::GetWindowDrawList()->AddLine({barMin.x, barBottom}, {barMin.x + ImGui::GetWindowWidth(), barBottom},
                                            ImGui::GetColorU32(palette.border));
    }

    if (captionButtonsStart > 0.0F)
        ImGui::PopClipRect();

    // WM_NCCALCSIZE makes this a fully client-rendered title bar.  ImGui draws
    // its glyphs while DWM and HT* caption results retain native behavior.
    const Editor::CaptionButtonBounds captionButtons = titleBar.captionButtonBounds();
    if (captionButtons.valid()) {
        ImDrawList* const drawList = ImGui::GetWindowDrawList();
        const float buttonWidth = (captionButtons.right - captionButtons.left) / 3.0F;
        const float buttonHeight = captionButtons.bottom - captionButtons.top;
        const ImVec2 mouse = ImGui::GetMousePos();
        const float glyphHalfExtent = std::round(5.0F * titleBar.dpiScale());
        const bool activeWindow = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        const ImU32 glyphColor = activeWindow ? IM_COL32(230, 235, 245, 255) : IM_COL32(230, 235, 245, 155);

        for (int index = 0; index < 3; ++index) {
            const ImVec2 minimum{captionButtons.left + buttonWidth * static_cast<float>(index), captionButtons.top};
            const ImVec2 maximum{minimum.x + buttonWidth, captionButtons.bottom};
            const bool hovered =
                mouse.x >= minimum.x && mouse.x < maximum.x && mouse.y >= minimum.y && mouse.y < maximum.y;
            const bool pressed = hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left);
            if (hovered) {
                const ImU32 hoverColor = index == 2 ? IM_COL32(196, 43, 28, activeWindow ? (pressed ? 255 : 235) : 150)
                                                    : IM_COL32(255, 255, 255, pressed ? 28 : 16);
                drawList->AddRectFilled(minimum, maximum, hoverColor);
            }

            const ImVec2 center{pixelAligned(minimum.x + buttonWidth * 0.5F),
                                pixelAligned(minimum.y + buttonHeight * 0.5F)};
            if (index == 0) {
                const float lineY = pixelAligned(center.y + std::round(2.0F * titleBar.dpiScale()));
                drawList->AddLine({pixelAligned(center.x - glyphHalfExtent), lineY},
                                  {pixelAligned(center.x + glyphHalfExtent), lineY}, glyphColor, 1.0F);
            } else if (index == 1) {
                if (titleBar.isMaximized()) {
                    drawRestoreGlyph(drawList, center, glyphHalfExtent, glyphColor);
                } else {
                    drawList->AddRect(
                        {pixelAligned(center.x - glyphHalfExtent), pixelAligned(center.y - glyphHalfExtent)},
                        {pixelAligned(center.x + glyphHalfExtent), pixelAligned(center.y + glyphHalfExtent)},
                        glyphColor, 0.0F, 0, 1.0F);
                }
            } else {
                drawList->AddLine({pixelAligned(center.x - glyphHalfExtent), pixelAligned(center.y - glyphHalfExtent)},
                                  {pixelAligned(center.x + glyphHalfExtent), pixelAligned(center.y + glyphHalfExtent)},
                                  glyphColor, 1.0F);
                drawList->AddLine({pixelAligned(center.x + glyphHalfExtent), pixelAligned(center.y - glyphHalfExtent)},
                                  {pixelAligned(center.x - glyphHalfExtent), pixelAligned(center.y + glyphHalfExtent)},
                                  glyphColor, 1.0F);
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
        ImGui::Spacing();
        ImGui::SetNextItemWidth(420.0F);
        ImGui::InputText("Name", newProjectName, sizeof(newProjectName));
        ImGui::SetNextItemWidth(420.0F);
        ImGui::InputTextWithHint("Location", "D:/Projects/MyGame", newProjectLocation, sizeof(newProjectLocation));
        ImGui::TextDisabled("Template");
        ImGui::SetNextItemWidth(420.0F);
        ImGui::BeginDisabled();
        char templateName[] = "3D Game";
        ImGui::InputText("##project-template", templateName, sizeof(templateName), ImGuiInputTextFlags_ReadOnly);
        ImGui::EndDisabled();
        ImGui::TextDisabled("Assets/Models, Materials, Textures, Scenes, Scripts and Audio will be created.");
        if (!newProjectError.empty()) {
            EditorUI::iconLabel(EditorUI::Icon::Error, EditorUI::colors().error);
            ImGui::SameLine(0.0F, 6.0F);
            ImGui::TextColored(EditorUI::colors().error, "%s", newProjectError.c_str());
        }
        ImGui::Spacing();
        const bool valid = newProjectName[0] != '\0' && newProjectLocation[0] != '\0';
        ImGui::BeginDisabled(!valid);
        if (EditorUI::primaryButton("Create", {110.0F, 0.0F})) {
            createProject();
            if (newProjectError.empty())
                ImGui::CloseCurrentPopup();
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
            antialiasingType = renderer.antialiasingLevel() == Engine::AntialiasingLevel::Off   ? 0
                               : renderer.antialiasingLevel() == Engine::AntialiasingLevel::TAA ? 3
                                                                                                : 1;
            msaaSamples = renderer.antialiasingLevel() == Engine::AntialiasingLevel::MSAA2x ? 2 : 4;
        }
        ImGui::OpenPopup("Scene Settings");
        openSceneSettings = false;
    }

    if (ImGui::BeginPopupModal("Scene Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        constexpr const char* typeLabels[] = {"None", "MSAA", "FXAA (placeholder)", "TAA"};
        constexpr const char* sampleLabels[] = {"2x", "4x"};

        ImGui::SeparatorText("Antialiasing");
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
                        renderer.setAntialiasingLevel(msaaSamples == 2 ? Engine::AntialiasingLevel::MSAA2x
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
                for (const int samples : {2, 4}) {
                    const bool isSelected = msaaSamples == samples;
                    if (ImGui::Selectable(samples == 2 ? sampleLabels[0] : sampleLabels[1], isSelected)) {
                        msaaSamples = samples;
                        renderer.setAntialiasingLevel(samples == 2 ? Engine::AntialiasingLevel::MSAA2x
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
        if (EditorButton("Close").draw()) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (showShortcuts) {
        ImGui::SetNextWindowSize({560.0F, 0.0F}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Keyboard Shortcuts", &showShortcuts, ImGuiWindowFlags_NoDocking)) {
            struct Shortcut final {
                const char* keys;
                const char* action;
            };
            static constexpr Shortcut sceneView[]{
                {"RMB", "Look around"}, {"RMB+W A S D", "Fly"}, {"Q / E", "Move down / up"},   {"Shift", "Move faster"},
                {"MMB", "Pan"},         {"Wheel", "Zoom"},      {"F", "Focus selected object"}};
            static constexpr Shortcut gizmos[]{{"W", "Move"},
                                               {"E", "Rotate"},
                                               {"R", "Scale"},
                                               {"Ctrl", "Snap: 0.25 m, 15 deg, 0.1 scale; snaps to nearby meshes"}};
            static constexpr Shortcut editing[]{{"Ctrl+Z", "Undo"},
                                                {"Ctrl+Y", "Redo"},
                                                {"Ctrl+C", "Copy object"},
                                                {"Ctrl+V", "Paste object"},
                                                {"Ctrl+D", "Duplicate object"},
                                                {"Delete", "Delete object"},
                                                {"Ctrl+Shift+N", "Create empty object"}};
            static constexpr Shortcut playMode[]{
                {"F5", "Play / Stop"}, {"F6", "Pause / Resume"}, {"Esc", "Release mouse"}};
            struct ShortcutGroup final {
                const char* title;
                std::span<const Shortcut> shortcuts;
            };
            const std::array groups{ShortcutGroup{"Scene View", sceneView}, ShortcutGroup{"Gizmos", gizmos},
                                    ShortcutGroup{"Editing", editing}, ShortcutGroup{"Play Mode", playMode}};
            for (const ShortcutGroup& group : groups) {
                ImGui::SeparatorText(group.title);
                if (ImGui::BeginTable(group.title, 2, ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("keys", ImGuiTableColumnFlags_WidthFixed, 150.0F);
                    ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthStretch);
                    for (const Shortcut& shortcut : group.shortcuts) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        EditorUI::keyChip(shortcut.keys);
                        ImGui::TableSetColumnIndex(1);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(shortcut.action);
                    }
                    ImGui::EndTable();
                }
            }
        }
        ImGui::End();
    }

    if (showAbout) {
        ImGui::SetNextWindowSize({420.0F, 0.0F}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("About GamEngine Editor", &showAbout, ImGuiWindowFlags_NoDocking)) {
            EditorUI::iconLabel(EditorUI::Icon::Mesh, EditorUI::colors().accent, ImGui::GetFontSize() * 1.4F);
            ImGui::SameLine();
            ImGui::TextUnformatted("GamEngine Editor");
            ImGui::TextDisabled("Experimental GPU-driven 3D engine built on Vulkan 1.4 and SDL3.");
            ImGui::Separator();
            ImGui::TextDisabled("Project");
            ImGui::SameLine(90.0F);
            ImGui::TextUnformatted(project.name().c_str());
            ImGui::TextDisabled("ImGui");
            ImGui::SameLine(90.0F);
            ImGui::TextUnformatted(IMGUI_VERSION);
        }
        ImGui::End();
    }

    return createdEntity;
}
