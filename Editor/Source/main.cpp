#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"

#include "Engine/Renderer/Renderer.h"
#include "Engine/Assets/Content.h"
#include "Engine/Assets/AssetTypes.h"
#include "Engine/Scene/ScenePresets.h"
#include "Engine/Scene/SceneEditor.h"
#include "Engine/Core/Time.h"
#include "Engine/Core/Transform.h"
#include "Engine/Core/Camera.h"
#include "Engine/Math/AABB.h"
#include "Engine/ECS/Components/ScriptComponent.h"
#include "Engine/ECS/Components/CameraComponent.h"
#include "Engine/ECS/Components/ColliderComponent.h"
#include "Engine/ECS/Components/RigidbodyComponent.h"
#include "Engine/ECS/Components/TerrainGrassComponent.h"
#include "Engine/ECS/Components/ColorPickerComponent.h"
#include "Engine/ECS/Components/SmokeEmitterComponent.h"
#include "Engine/ECS/Components/ProceduralCloudComponent.h"
#include "Engine/Renderer/Geometry/ProceduralCloud.h"
#include "Engine/Scene/Components/LightComponent.h"
#include "Engine/Renderer/MeshRenderer.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scripting/ScriptSystem.h"
#include "Engine/Physics/PhysicsSystem.h"
#include "Engine/Project.h"
#include "Elements/EditorButton.h"
#include "Elements/NumericControl.h"
#include "Elements/TransformFields.h"
#include "Editor/Panels/EditorSceneSession.h"
#include "Editor/Panels/EditorStyle.h"
#include "Editor/Panels/HierarchyPanel.h"
#include "Editor/Panels/ComponentsPanel.h"
#include "Editor/Panels/AssetManagerPanel.h"
#include "Editor/Panels/ConsolePanel.h"
#include "Editor/Panels/AssetDragDrop.h"
#include "Editor/EditorState.h"
#include "Editor/EditorPreferences.h"
#include "Editor/EditorConstants.h"
#include "Editor/EditorUi.h"
#include "Editor/TerrainSculptState.h"

using Editor::EntityClipboard;
using Editor::SceneHistory;

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdint>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "EditorEntityHelpers.inl"

#include "EditorViewport.inl"

#include "TerrainToolsPanel.inl"

#include "HierarchyPanel.inl"

#include "ComponentsPanel.inl"

#include "EditorShell.inl"

namespace {
    std::filesystem::path findDefaultUiFont() {
        constexpr std::array<const char *, 5> candidates{
            "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
            "C:/Windows/Fonts/consola.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
        };
        const auto font = std::ranges::find_if(candidates, [](const char *path) {
            return std::filesystem::is_regular_file(path);
        });
        if (font == candidates.end()) {
            throw std::runtime_error("No default TrueType font found for ImGui");
        }
        return *font;
    }
}
// NOLINTBEGIN(readability-magic-numbers)

int main(int argc, char** argv) {
    try {
        std::optional<std::filesystem::path> projectPath;
        std::optional<std::filesystem::path> createProjectPath;
        for (int index = 1; index < argc; ++index) {
            if (std::string_view{argv[index]} == "--project") {
                if (++index == argc) throw std::runtime_error("--project requires a file path");
                projectPath = argv[index];
            } else if (std::string_view{argv[index]} == "--create-project") {
                if (++index == argc) throw std::runtime_error("--create-project requires a directory path");
                createProjectPath = argv[index];
            }
        }
        if (projectPath && createProjectPath) {
            throw std::runtime_error("Use either --project or --create-project, not both");
        }
        const Editor::EditorSession previousSession = Editor::loadSession();
        Engine::Project project = createProjectPath
                                            ? Engine::Project::create(*createProjectPath)
                                            : projectPath
                                            ? Engine::Project::load(*projectPath)
                                            : std::filesystem::is_regular_file(previousSession.projectManifest)
                                            ? Engine::Project::load(previousSession.projectManifest)
                                            : [&] {
                                                  try {
                                                      return Engine::Project::discover(
                                                          std::filesystem::current_path());
                                                  } catch (const std::runtime_error&) {
                                                      return Engine::Project::defaults(
                                                          std::filesystem::path{GAMEENGINE_SOURCE_DIR});
                                                  }
                                              }();
        EditorSceneSession::setProjectRoot(project.rootPath());
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
            throw std::runtime_error(SDL_GetError());
        }
        SDL_Window *window = SDL_CreateWindow((project.name() + " Editor").c_str(),
                                              EditorConstants::windowWidth,
                                              EditorConstants::windowHeight,
                                              SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
        if (window == nullptr) {
            throw std::runtime_error(SDL_GetError());
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &imguiIo = ImGui::GetIO();
        const std::filesystem::path preferencesPath = Editor::preferencesDirectory();
        std::filesystem::create_directories(preferencesPath);
        const std::string imguiIniPath = (preferencesPath / "imgui.ini").string();
        const bool restorePersistedLayout = std::filesystem::is_regular_file(imguiIniPath);
        imguiIo.IniFilename = imguiIniPath.c_str();
        imguiIo.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
        const std::filesystem::path uiFont = findDefaultUiFont();
        if (imguiIo.Fonts->AddFontFromFileTTF(uiFont.string().c_str(), 16.0F) == nullptr) {
            throw std::runtime_error("Could not load ImGui font: " + uiFont.string());
        }
        EditorStyle::apply();

        Engine::ScenePreset scene;
        Engine::Assets::Content content{project.assetRoot()};
        content.setErrorHandler([](const std::string& message) {
            Editor::ConsolePanel::error("Asset: " + message);
        });
        Editor::ConsolePanel::info("Editor started for project '" + project.name() + "'.");
        const bool restoreScene = !projectPath && !createProjectPath &&
                                  std::filesystem::is_regular_file(previousSession.scenePath);
        const auto initialScene = restoreScene
                                      ? previousSession.scenePath
                                      : project.startupScene();
        if (std::filesystem::is_regular_file(initialScene)) {
            Engine::SceneSerializer::load(scene, initialScene);
        }
        EditorSceneSession::setScenePath(initialScene);
        Engine::ScriptSystem scriptSystem{Engine::ScriptRegistry::instance()};
        Engine::PhysicsSystem physicsSystem{};
        // The editor is the visual authoring path, so shadows must be active
        // by default. The engine-level default stays conservative for clients
        // which explicitly optimize for an unshadowed renderer.
        Engine::Renderer renderer{Engine::RenderConfig{
            .features = Engine::RenderFeatures{.shadows = true}}};
        renderer.initialize(scene, window);
        SceneHistory history;
        history.reset(scene);
        EntityClipboard clipboard;
        Engine::Entity selectedEntity = Engine::NullEntity;
        std::vector<Engine::Entity> selectedEntities;
        const auto setSelection = [&](const Engine::Entity entity) {
            selectedEntities.clear();
            if (entity != Engine::NullEntity) selectedEntities.push_back(entity);
            selectedEntity = entity;
            renderer.setEditorSelection(selectedEntity);
        };
        const auto toggleSelection = [&](const Engine::Entity entity) {
            if (const auto found = std::ranges::find(selectedEntities, entity);
                found != selectedEntities.end()) {
                selectedEntities.erase(found);
                if (selectedEntity == entity)
                    selectedEntity = selectedEntities.empty() ? Engine::NullEntity : selectedEntities.back();
            } else {
                selectedEntities.push_back(entity);
                selectedEntity = entity;
            }
            renderer.setEditorSelection(selectedEntity);
        };
        constexpr auto targetFrame = EditorConstants::targetFrameMicroseconds;
        bool running = true;
        bool rendererReloadPending = false;
        bool playing = false;
        bool paused = false;
        bool showHierarchy = true;
        bool showViewport = true;
        bool showInspector = true;
        bool showAssetManager = true;
        bool showTerrainTools = true;
        bool showConsole = true;
        double physicsAccumulator = 0.0;
        bool showGameView = false;
        GizmoMode gizmoMode = GizmoMode::Translate;
        SelectionTool selectionTool = SelectionTool::Rectangle;
        TerrainSculptState terrainSculpt;
        std::string playSceneSnapshot;
        std::string playModeError;
        constexpr auto autoSaveInterval = std::chrono::seconds{30};
        auto lastAutoSaveAttempt = std::chrono::steady_clock::now();
        std::uint64_t lastPersistedSceneRevision = scene.editor().mutationRevision();
        while (running) {
            const auto start = std::chrono::steady_clock::now();
            Engine::Renderer::beginFrame();
            const Engine::EditorEventState events = renderer.pollEditorEvents();
            if (events.quitRequested) {
                running = false;
            }
            // Apply Scene View navigation before drawing its gizmo. Rendering
            // later in this frame then uses this exact same camera transform.
            renderer.updateEditorSceneCameraInput();
            if (events.togglePlay) {
                if (EditorSceneSession::setPlayMode(!playing, scene, playSceneSnapshot, playModeError,
                                                    EditorSceneSession::msaaSampleCount(renderer))) {
                    playing = !playing;
                    paused = false;
                    physicsAccumulator = 0.0;
                    showGameView = playing;
                    rendererReloadPending = !playing;
                    Editor::ConsolePanel::info(playing ? "Entered Play mode." : "Stopped Play mode.");
                } else Editor::ConsolePanel::error("Could not change Play mode: " + playModeError);
            }
            if (events.togglePause && playing) {
                paused = !paused;
                physicsAccumulator = 0.0;
            }
            if (!running) {
                break;
            }

            // Antialiasing changes recreate render-target resources. Ordinary
            // scene edits are synchronized below without rebuilding the UI or
            // swapchain.
            if (rendererReloadPending) {
                renderer.reloadScene(scene, window);
                rendererReloadPending = false;
            }

            const std::uint64_t sceneStructureBeforeUi = scene.editor().structuralRevision();
            renderer.beginEditorUiFrame();
            bool antialiasingChanged = false;
            bool sceneLoaded = false;
            bool sceneSaved = false;
            bool playToggleRequested = false;
            bool pauseToggleRequested = false;
            bool undoRequested = false;
            bool redoRequested = false;
            bool copyRequested = false;
            bool pasteRequested = false;
            bool duplicateRequested = false;
            bool resetHistoryRequested = false;
            if (const Engine::Entity created = drawEditorMenuBar(scene, renderer, content, project,
                                                                 antialiasingChanged, sceneLoaded, sceneSaved,
                                                                 playing, paused, playToggleRequested,
                                                                 pauseToggleRequested, history.canUndo(),
                                                                 history.canRedo(), clipboard.canPaste(scene),
                                                                 undoRequested, redoRequested, copyRequested,
                                                                 pasteRequested, duplicateRequested,
                                                                 resetHistoryRequested, showHierarchy,
                                                                 showViewport, showInspector, showAssetManager,
                                                                 showTerrainTools, showConsole);
                created != Engine::NullEntity) {
                setSelection(created);
            }
            if (resetHistoryRequested) {
                history.reset(scene);
            }
            if (!playing && undoRequested && history.undo(scene)) {
                sceneLoaded = true;
            }
            if (!playing && redoRequested && history.redo(scene)) {
                sceneLoaded = true;
            }
            if (sceneLoaded) {
                // Loading replaces the registry, so any selection from the
                // previous scene is stale before the hierarchy/inspector are
                // drawn for this frame.
                setSelection(Engine::NullEntity);
                rendererReloadPending = true;
            }
            if (!playing && selectedEntity != Engine::NullEntity &&
                scene.editor().valid(selectedEntity)) {
                if (copyRequested) {
                    clipboard.copy(scene, selectedEntity);
                }
                if (pasteRequested) {
                    setSelection(clipboard.paste(scene));
                }
                if (duplicateRequested) {
                    setSelection(scene.editor().duplicate(selectedEntity));
                }
            }
            const auto setPlayMode = [&](const bool enabled) {
                if (!EditorSceneSession::setPlayMode(enabled, scene, playSceneSnapshot, playModeError,
                                                     EditorSceneSession::msaaSampleCount(renderer))) {
                    Editor::ConsolePanel::error("Could not change Play mode: " + playModeError);
                    return false;
                }
                playing = enabled;
                paused = false;
                physicsAccumulator = 0.0;
                showGameView = playing;
                if (!playing) {
                    setSelection(Engine::NullEntity);
                    rendererReloadPending = true;
                }
                Editor::ConsolePanel::info(playing ? "Entered Play mode." : "Stopped Play mode.");
                return true;
            };
            if (playToggleRequested) static_cast<void>(setPlayMode(!playing));
            if (pauseToggleRequested && playing) {
                paused = !paused;
                physicsAccumulator = 0.0;
            }
            const ImGuiViewport *viewport = ImGui::GetMainViewport();
            const ImVec2 dockSize{viewport->WorkSize.x,
                                  std::max(0.0F, viewport->WorkSize.y -
                                                    static_cast<float>(EditorConstants::statusBarHeight))};
            // Keep the status bar outside the dockspace. DockSpaceOverViewport
            // uses the complete work area, which allowed docked panels to
            // continue underneath the status bar.
            ImGui::SetNextWindowPos(viewport->WorkPos);
            ImGui::SetNextWindowSize(dockSize);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0F, 0.0F});
            ImGui::Begin("##editor-dockspace", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground);
            const ImGuiID dockspaceId = viewport->ID;
            ImGui::DockSpace(dockspaceId, {0.0F, 0.0F}, ImGuiDockNodeFlags_PassthruCentralNode);
            EditorStyle::configureDockLayout(dockSize, restorePersistedLayout);
            ImGui::End();
            ImGui::PopStyleVar();
            HierarchyPanel::Action hierarchyAction = HierarchyPanel::Action::None;
            Engine::Entity hierarchyActionEntity = Engine::NullEntity;
            if (showHierarchy) {
                if (const Engine::Entity clicked = HierarchyPanel::draw(
                        scene, content, selectedEntities, hierarchyAction, hierarchyActionEntity,
                        clipboard.canPaste(scene), playing, showHierarchy);
                    clicked != Engine::NullEntity) {
                    // Ctrl toggles individual rows; Shift extends the set.
                    // The last clicked object remains the active inspector/gizmo target.
                    if (ImGui::GetIO().KeyCtrl) toggleSelection(clicked);
                    else if (ImGui::GetIO().KeyShift) {
                        if (std::ranges::find(selectedEntities, clicked) == selectedEntities.end())
                            selectedEntities.push_back(clicked);
                        selectedEntity = clicked;
                        renderer.setEditorSelection(selectedEntity);
                    } else setSelection(clicked);
                }
            }
            if (!playing && hierarchyAction == HierarchyPanel::Action::Paste) {
                setSelection(clipboard.paste(scene));
            } else if (!playing && hierarchyActionEntity != Engine::NullEntity &&
                       scene.editor().valid(hierarchyActionEntity)) {
                if (hierarchyAction == HierarchyPanel::Action::Delete) {
                    scene.editor().destroy(hierarchyActionEntity);
                    if (selectedEntity == hierarchyActionEntity) {
                        setSelection(Engine::NullEntity);
                    }
                } else if (hierarchyAction == HierarchyPanel::Action::Duplicate) {
                    setSelection(scene.editor().duplicate(hierarchyActionEntity));
                } else if (hierarchyAction == HierarchyPanel::Action::Copy) {
                    clipboard.copy(scene, hierarchyActionEntity);
                }
            }
            if (!playing && !ImGui::GetIO().WantTextInput &&
                !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                bool gizmoModeChanged = false;
                if (ImGui::IsKeyPressed(ImGuiKey_W)) {
                    gizmoMode = GizmoMode::Translate;
                    gizmoModeChanged = true;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_E)) {
                    gizmoMode = GizmoMode::Rotate;
                    gizmoModeChanged = true;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_R)) {
                    gizmoMode = GizmoMode::Scale;
                    gizmoModeChanged = true;
                }
                if (gizmoModeChanged && selectedEntity != Engine::NullEntity &&
                    scene.editor().valid(selectedEntity)) {
                    renderer.setEditorSelection(selectedEntity);
                }
            }
            ViewportInteraction viewportInteraction{};
            if (showViewport) {
                viewportInteraction = drawViewport(
                    scene, content, selectedEntity, selectedEntities, renderer,
                    renderer.gameViewport(), renderer.sceneViewport(),
                    renderer.editorCameraYaw(), renderer.editorCameraPitch(), showGameView, gizmoMode,
                    selectionTool, terrainSculpt, playing, showViewport);
            }
            if (viewportInteraction.playModeAction == PlayModeAction::Start && !playing) {
                static_cast<void>(setPlayMode(true));
            } else if (viewportInteraction.playModeAction == PlayModeAction::Stop && playing) {
                static_cast<void>(setPlayMode(false));
            } else if (viewportInteraction.playModeAction == PlayModeAction::Restart && playing &&
                       setPlayMode(false)) {
                static_cast<void>(setPlayMode(true));
            }
            if (showTerrainTools) {
                drawTerrainToolsPanel(scene, content, selectedEntity, renderer, terrainSculpt,
                                      playing, showTerrainTools);
            }
            if (viewportInteraction.createdEntity != Engine::NullEntity) {
                setSelection(viewportInteraction.createdEntity);
            }
            if (!playing && viewportInteraction.sceneClicked) {
                constexpr float viewportAspect = EditorConstants::viewportWidthRatio /
                                                 EditorConstants::viewportHeightRatio;
                const Engine::Entity hit = pickSceneEntity(scene, physicsSystem, renderer,
                                                           viewportInteraction.normalizedX,
                                                           viewportInteraction.normalizedY, viewportAspect);
                if (ImGui::GetIO().KeyCtrl && hit != Engine::NullEntity) toggleSelection(hit);
                else if (ImGui::GetIO().KeyShift && hit != Engine::NullEntity) {
                    if (std::ranges::find(selectedEntities, hit) == selectedEntities.end()) selectedEntities.push_back(hit);
                    selectedEntity = hit;
                    renderer.setEditorSelection(selectedEntity);
                } else setSelection(hit);
            }
            if (!playing && viewportInteraction.selectionCommitted) {
                if (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift) {
                    for (const Engine::Entity entity : viewportInteraction.selectedEntities) {
                        if (std::ranges::find(selectedEntities, entity) == selectedEntities.end())
                            selectedEntities.push_back(entity);
                    }
                    if (!selectedEntities.empty()) selectedEntity = selectedEntities.back();
                    renderer.setEditorSelection(selectedEntity);
                } else {
                    selectedEntities = std::move(viewportInteraction.selectedEntities);
                    selectedEntity = selectedEntities.empty() ? Engine::NullEntity : selectedEntities.back();
                    renderer.setEditorSelection(selectedEntity);
                }
            }
            const bool inspectorConsumesMouseWheel = showInspector &&
                ComponentsPanel::draw(scene, selectedEntities, selectedEntity, showInspector);
            if (showAssetManager) {
                if (const Engine::Entity created =
                        AssetManagerPanel::draw(scene, content, playing, showAssetManager,
                                                !project.manifestPath().empty());
                    created != Engine::NullEntity) {
                    setSelection(created);
                }
            }
            if (showConsole) Editor::ConsolePanel::draw(showConsole);
            drawStatusBar(scene, selectedEntity, playing, paused);
            if (!playing && selectedEntity != Engine::NullEntity &&
                scene.editor().valid(selectedEntity) && !ImGui::GetIO().WantTextInput &&
                ImGui::IsKeyPressed(ImGuiKey_Delete)) {
                scene.editor().destroy(selectedEntity);
                setSelection(Engine::NullEntity);
            }
            if (!playing && !ImGui::GetIO().WantTextInput &&
                ImGui::GetIO().KeyCtrl) {
                if (ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_N)) {
                    selectedEntity = scene.createGameObject();
                    renderer.setEditorSelection(selectedEntity);
                }
                if (ImGui::IsKeyPressed(ImGuiKey_Z) && history.undo(scene)) {
                    sceneLoaded = true;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_Y) && history.redo(scene)) {
                    sceneLoaded = true;
                }
                if (selectedEntity != Engine::NullEntity && scene.editor().valid(selectedEntity)) {
                    if (ImGui::IsKeyPressed(ImGuiKey_C)) {
                        clipboard.copy(scene, selectedEntity);
                    }
                    if (ImGui::IsKeyPressed(ImGuiKey_D)) {
                        setSelection(scene.editor().duplicate(selectedEntity));
                    }
                    if (ImGui::IsKeyPressed(ImGuiKey_V)) {
                        setSelection(clipboard.paste(scene));
                    }
                }
                if (sceneLoaded) {
                    setSelection(Engine::NullEntity);
                    rendererReloadPending = true;
                }
            }
            renderer.setEditorSceneCameraInput(
                viewportInteraction.cameraInput && !inspectorConsumesMouseWheel);
            // A Stop click changes `playing` after drawViewport() has produced
            // its interaction state.  Do not pass that stale state to the
            // camera controller: it would interpret the same click as a
            // request to capture the game mouse again.
            renderer.setGameCameraInput(playing && viewportInteraction.gameCameraInput);
            if (playing && viewportInteraction.gameMouseCaptureRequested) {
                renderer.requestGameMouseCapture();
            }
            renderer.setSceneViewportActive(showViewport && !showGameView && !playing);

            if (antialiasingChanged) {
                rendererReloadPending = true;
            }
            const bool sceneStructureChanged = !sceneLoaded &&
                                               scene.editor().structuralRevision() != sceneStructureBeforeUi;
            // ImGui::Image has already captured this frame's viewport descriptor.
            // Rebuilding scene resources can release that descriptor, so defer the
            // rebuild until its draw commands have been submitted below.
            const bool sceneResourceSyncPending = sceneStructureChanged ||
                                                  viewportInteraction.terrainGrassChanged;
            if (!sceneResourceSyncPending && viewportInteraction.terrainGeometryChanged) {
                for (const Engine::Entity terrain : viewportInteraction.terrainGeometryEntities)
                    renderer.updateMeshGeometry(terrain);
            }
            if (terrainSculpt.strokeCompleted) {
                static_cast<void>(history.capture(scene));
                terrainSculpt.strokeCompleted = false;
                terrainSculpt.completedEntity = Engine::NullEntity;
                terrainSculpt.completedDirty = {};
                terrainSculpt.heightsBeforeStroke.clear();
            }
            // A history snapshot serializes the complete scene, including
            // decoded GLB image pixels. Capture only after an actual mutation
            // and once an interactive edit has finished; SceneHistory performs
            // the revision check before touching the serializer.
            const bool editingScene = ImGui::IsAnyItemActive() ||
                                      ImGui::IsMouseDown(ImGuiMouseButton_Left);
            if (!playing && !sceneLoaded && !editingScene) {
                static_cast<void>(history.capture(scene));
            }
            if (sceneSaved) {
                lastPersistedSceneRevision = scene.editor().mutationRevision();
                lastAutoSaveAttempt = std::chrono::steady_clock::now();
            }
            const auto now = std::chrono::steady_clock::now();
            const std::uint64_t currentSceneRevision = scene.editor().mutationRevision();
            if (!playing && EditorSceneSession::hasSavedScene() &&
                currentSceneRevision != lastPersistedSceneRevision &&
                now - lastAutoSaveAttempt >= autoSaveInterval) {
                lastAutoSaveAttempt = now;
                try {
                    const std::filesystem::path path = EditorSceneSession::scenePath();
                    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
                    Engine::SceneSerializer::save(scene, path,
                                                  EditorSceneSession::msaaSampleCount(renderer));
                    EditorSceneSession::markSceneSaved(path);
                    lastPersistedSceneRevision = currentSceneRevision;
                    Editor::ConsolePanel::info("Auto-saved scene: " + path.string());
                } catch (const std::exception& error) {
                    Editor::ConsolePanel::warning("Could not auto-save scene: " +
                                                  std::string{error.what()});
                }
            }
            if (playing && !paused) {
                physicsAccumulator += Engine::Time::deltaTime();
                int physicsSteps = 0;
                while (physicsAccumulator >= EditorConstants::physicsStep &&
                       physicsSteps < EditorConstants::maximumPhysicsStepsPerFrame) {
                    physicsSystem.update(scene, static_cast<float>(EditorConstants::physicsStep));
                    physicsAccumulator -= EditorConstants::physicsStep;
                    ++physicsSteps;
                }
                scriptSystem.update(scene, static_cast<float>(Engine::Time::deltaTime()));
            }
            ImGui::Render();
            renderer.renderFrame();

            if (sceneResourceSyncPending) {
                renderer.synchronizeScene(scene);
                // Duplicated objects do not exist in the renderer's cached
                // renderable list until synchronization completes. Reapply
                // the selection so the next frame can outline it immediately.
                if (sceneStructureChanged) renderer.setEditorSelection(selectedEntity);
            }

            // Keep the editor UI responsive without unnecessarily throttling
            // the game simulation while Play Mode is active.
            if (const auto elapsed = std::chrono::steady_clock::now() - start;
                !playing && elapsed < targetFrame) {
                std::this_thread::sleep_for(targetFrame - elapsed);
            }
        }
        Editor::saveSession({.projectManifest = project.manifestPath(),
                             .scenePath = EditorSceneSession::scenePath()});
        renderer.shutdown();
        ImGui::DestroyContext();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Editor error: %s\n", error.what());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "GamEngine Editor error",
                                 error.what(), nullptr);
        return 1;
    }
}

// NOLINTEND(readability-magic-numbers)
