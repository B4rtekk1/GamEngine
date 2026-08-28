#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"

#include "Engine/Renderer/Renderer.h"
#include "Engine/Assets/Content.h"
#include "Engine/Scene/ScenePresets.h"
#include "Engine/Scene/SceneEditor.h"
#include "Engine/Core/Time.h"
#include "Engine/Core/Transform.h"
#include "Engine/Core/Camera.h"
#include "Engine/ECS/Components/ScriptComponent.h"
#include "Engine/ECS/Components/CameraComponent.h"
#include "Engine/ECS/Components/ColliderComponent.h"
#include "Engine/ECS/Components/RigidbodyComponent.h"
#include "Engine/ECS/Components/ColorPickerComponent.h"
#include "Engine/ECS/Components/SmokeEmitterComponent.h"
#include "Engine/Scene/Components/LightComponent.h"
#include "Engine/Renderer/MeshRenderer.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scripting/ScriptSystem.h"
#include "Engine/Physics/PhysicsSystem.h"
#include "Elements/EditorButton.h"
#include "Elements/TransformFields.h"
#include "Editor/Panels/EditorSceneSession.h"
#include "Editor/Panels/EditorStyle.h"
#include "Editor/Panels/HierarchyPanel.h"
#include "Editor/Panels/ComponentsPanel.h"
#include "Editor/Panels/AssetManagerPanel.h"
#include "Editor/Panels/AssetDragDrop.h"
#include "Editor/EditorState.h"
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
#include <thread>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "EditorEntityHelpers.inl"

#include "EditorViewport.inl"

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
int main() {
    try {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
            throw std::runtime_error(SDL_GetError());
        }
        SDL_Window *window = SDL_CreateWindow("GamEngine Editor",
                                              EditorConstants::windowWidth,
                                              EditorConstants::windowHeight,
                                              SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
        if (window == nullptr) {
            throw std::runtime_error(SDL_GetError());
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &imguiIo = ImGui::GetIO();
        imguiIo.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
        const std::filesystem::path uiFont = findDefaultUiFont();
        if (imguiIo.Fonts->AddFontFromFileTTF(uiFont.string().c_str(), 16.0F) == nullptr) {
            throw std::runtime_error("Could not load ImGui font: " + uiFont.string());
        }
        EditorStyle::apply();

        Engine::ScenePreset scene;
        Engine::Assets::Content content{
            std::filesystem::path{GAMEENGINE_SOURCE_DIR} / "Assets"};
        Engine::ScriptSystem scriptSystem{Engine::ScriptRegistry::instance()};
        Engine::PhysicsSystem physicsSystem{};
        Engine::Renderer renderer;
        renderer.initialize(scene, window);
        SceneHistory history;
        history.reset(scene);
        EntityClipboard clipboard;
        Engine::Entity selectedEntity = Engine::NullEntity;
        constexpr auto targetFrame = EditorConstants::targetFrameMicroseconds;
        bool running = true;
        bool rendererReloadPending = false;
        bool playing = false;
        bool paused = false;
        bool showHierarchy = true;
        bool showViewport = true;
        bool showInspector = true;
        bool showAssetManager = true;
        double physicsAccumulator = 0.0;
        bool showGameView = false;
        GizmoMode gizmoMode = GizmoMode::Translate;
        TerrainSculptState terrainSculpt;
        std::string playSceneSnapshot;
        std::string playModeError;
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
                }
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
            bool playToggleRequested = false;
            bool pauseToggleRequested = false;
            bool undoRequested = false;
            bool redoRequested = false;
            bool copyRequested = false;
            bool pasteRequested = false;
            bool duplicateRequested = false;
            bool resetHistoryRequested = false;
            if (const Engine::Entity created = drawEditorMenuBar(scene, renderer,
                                                                 antialiasingChanged, sceneLoaded,
                                                                 playing, paused, playToggleRequested,
                                                                 pauseToggleRequested, history.canUndo(),
                                                                 history.canRedo(), clipboard.canPaste(scene),
                                                                 undoRequested, redoRequested, copyRequested,
                                                                 pasteRequested, duplicateRequested,
                                                                 resetHistoryRequested, showHierarchy,
                                                                 showViewport, showInspector, showAssetManager);
                created != Engine::NullEntity) {
                selectedEntity = created;
                renderer.setEditorSelection(selectedEntity);
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
                selectedEntity = Engine::NullEntity;
                renderer.setEditorSelection(selectedEntity);
                rendererReloadPending = true;
            }
            if (!playing && selectedEntity != Engine::NullEntity &&
                scene.editor().valid(selectedEntity)) {
                if (copyRequested) {
                    clipboard.copy(scene, selectedEntity);
                }
                if (pasteRequested) {
                    selectedEntity = clipboard.paste(scene);
                    renderer.setEditorSelection(selectedEntity);
                }
                if (duplicateRequested) {
                    selectedEntity = scene.editor().duplicate(selectedEntity);
                    renderer.setEditorSelection(selectedEntity);
                }
            }
            if (playToggleRequested && EditorSceneSession::setPlayMode(!playing, scene, playSceneSnapshot,
                                                                       playModeError,
                                                                       EditorSceneSession::msaaSampleCount(renderer))) {
                playing = !playing;
                paused = false;
                physicsAccumulator = 0.0;
                showGameView = playing;
                if (!playing) {
                    selectedEntity = Engine::NullEntity;
                    renderer.setEditorSelection(selectedEntity);
                    rendererReloadPending = true;
                }
            }
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
            EditorStyle::configureDockLayout(dockSize);
            ImGui::End();
            ImGui::PopStyleVar();
            HierarchyPanel::Action hierarchyAction = HierarchyPanel::Action::None;
            Engine::Entity hierarchyActionEntity = Engine::NullEntity;
            if (showHierarchy) {
                if (const Engine::Entity clicked = HierarchyPanel::draw(
                        scene, content, selectedEntity, hierarchyAction, hierarchyActionEntity,
                        clipboard.canPaste(scene), showHierarchy);
                    clicked != Engine::NullEntity) {
                    selectedEntity = clicked;
                    renderer.setEditorSelection(selectedEntity);
                }
            }
            if (!playing && hierarchyAction == HierarchyPanel::Action::Paste) {
                selectedEntity = clipboard.paste(scene);
                renderer.setEditorSelection(selectedEntity);
            } else if (!playing && hierarchyActionEntity != Engine::NullEntity &&
                       scene.editor().valid(hierarchyActionEntity)) {
                if (hierarchyAction == HierarchyPanel::Action::Delete) {
                    scene.editor().destroy(hierarchyActionEntity);
                    if (selectedEntity == hierarchyActionEntity) {
                        selectedEntity = Engine::NullEntity;
                        renderer.setEditorSelection(selectedEntity);
                    }
                } else if (hierarchyAction == HierarchyPanel::Action::Duplicate) {
                    selectedEntity = scene.editor().duplicate(hierarchyActionEntity);
                    renderer.setEditorSelection(selectedEntity);
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
                if (gizmoModeChanged && selectedEntity != Engine::NullEntity &&
                    scene.editor().valid(selectedEntity)) {
                    renderer.setEditorSelection(selectedEntity);
                }
            }
            ViewportInteraction viewportInteraction{};
            if (showViewport) {
                viewportInteraction = drawViewport(
                    scene, content, selectedEntity, renderer, renderer.gameViewport(), renderer.sceneViewport(),
                    renderer.editorCameraYaw(), renderer.editorCameraPitch(), showGameView, gizmoMode,
                    terrainSculpt, playing, showViewport);
            }
            if (viewportInteraction.createdEntity != Engine::NullEntity) {
                selectedEntity = viewportInteraction.createdEntity;
                renderer.setEditorSelection(selectedEntity);
            }
            if (!playing && viewportInteraction.sceneClicked) {
                constexpr float viewportAspect = EditorConstants::viewportWidthRatio /
                                                 EditorConstants::viewportHeightRatio;
                selectedEntity = pickSceneEntity(scene, physicsSystem, renderer,
                                                 viewportInteraction.normalizedX,
                                                 viewportInteraction.normalizedY, viewportAspect);
                renderer.setEditorSelection(selectedEntity);
            }
            const bool inspectorConsumesMouseWheel = showInspector &&
                ComponentsPanel::draw(scene, selectedEntity, showInspector);
            if (showAssetManager) {
                if (const Engine::Entity created =
                        AssetManagerPanel::draw(scene, content, playing, showAssetManager);
                    created != Engine::NullEntity) {
                    selectedEntity = created;
                    renderer.setEditorSelection(selectedEntity);
                }
            }
            drawStatusBar(scene, selectedEntity, playing, paused);
            if (!playing && selectedEntity != Engine::NullEntity &&
                scene.editor().valid(selectedEntity) && !ImGui::GetIO().WantTextInput &&
                ImGui::IsKeyPressed(ImGuiKey_Delete)) {
                scene.editor().destroy(selectedEntity);
                selectedEntity = Engine::NullEntity;
                renderer.setEditorSelection(selectedEntity);
            }
            if (!playing && !ImGui::GetIO().WantTextInput &&
                ImGui::GetIO().KeyCtrl) {
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
                        selectedEntity = scene.editor().duplicate(selectedEntity);
                        renderer.setEditorSelection(selectedEntity);
                    }
                    if (ImGui::IsKeyPressed(ImGuiKey_V)) {
                        selectedEntity = clipboard.paste(scene);
                        renderer.setEditorSelection(selectedEntity);
                    }
                }
                if (sceneLoaded) {
                    selectedEntity = Engine::NullEntity;
                    renderer.setEditorSelection(selectedEntity);
                    rendererReloadPending = true;
                }
            }
            renderer.setEditorSceneCameraInput(
                viewportInteraction.cameraInput && !inspectorConsumesMouseWheel);
            ImGui::Render();

            if (antialiasingChanged) {
                rendererReloadPending = true;
            }
            const bool sceneStructureChanged = !sceneLoaded &&
                                               scene.editor().structuralRevision() != sceneStructureBeforeUi;
            if (sceneStructureChanged) {
                renderer.synchronizeScene(scene);
                // Duplicated objects do not exist in the renderer's cached
                // renderable list until synchronization completes. Reapply
                // the selection afterwards so the new copy can be outlined
                // and selected in the Scene View immediately.
                renderer.setEditorSelection(selectedEntity);
            } else if (viewportInteraction.terrainGeometryChanged) {
                renderer.updateMeshGeometry(selectedEntity, viewportInteraction.terrainFirstVertex,
                                            viewportInteraction.terrainVertexCount);
            }
            if (terrainSculpt.strokeCompleted) {
                static_cast<void>(history.captureTerrainStroke(
                    scene, terrainSculpt.completedEntity, terrainSculpt.heightsBeforeStroke,
                    terrainSculpt.completedDirty));
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
            renderer.renderFrame();

            if (const auto elapsed = std::chrono::steady_clock::now() - start; elapsed < targetFrame) {
                std::this_thread::sleep_for(targetFrame - elapsed);
            }
        }
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
