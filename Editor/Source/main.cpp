#include "imgui.h"
#include "imgui_internal.h"

#include "Engine/Renderer/Vulkan/renderer.h"
#include "Engine/Scene/Scene.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace {

bool drawViewport(VkDescriptorSet gameDescriptor, VkDescriptorSet sceneDescriptor) {
    static bool showGameView = false;
    const VkDescriptorSet descriptor = showGameView ? gameDescriptor : sceneDescriptor;

    ImGui::Begin("Viewport");
    if (ImGui::Button(showGameView ? "Show Scene View" : "Show Game View")) {
        showGameView = !showGameView;
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(showGameView ? "Game camera" : "Scene camera");
    ImGui::SameLine();
    ImGui::TextDisabled("shared renderer | shadows disabled");
    const bool sceneCameraInput = !showGameView && ImGui::IsWindowHovered();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    if (descriptor != VK_NULL_HANDLE && size.x > 1.0f && size.y > 1.0f) {
        ImVec2 imageSize = size;
        if (showGameView) {
            constexpr float gameAspect = 16.0f / 9.0f;
            imageSize.x = std::min(size.x, size.y * gameAspect);
            imageSize.y = imageSize.x / gameAspect;
            // Keep unused space as letterboxing around the fixed game frame.
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (size.x - imageSize.x) * 0.5f);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (size.y - imageSize.y) * 0.5f);
        }
        ImGui::Image(ImTextureRef{static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(descriptor))},
                     imageSize, {0, 0}, {1, 1});
    }
    ImGui::End();
    return sceneCameraInput;
}

Engine::Entity drawHierarchy(const Engine::Scene& scene, const Engine::Entity selected) {
    Engine::Entity clicked = Engine::NullEntity;
    ImGui::Begin("Hierarchy");

    if (ImGui::TreeNodeEx("Scene", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth)) {
        const auto entityLabel = [&](const char* name, const Engine::Entity entity) {
            if (entity != Engine::NullEntity) {
                char label[64];
                std::snprintf(label, sizeof(label), "%s  (%u)", name,
                              Engine::entityIndex(entity));
                if (ImGui::Selectable(label, selected == entity)) clicked = entity;
            }
        };

        entityLabel("Plane", scene.plane);
        entityLabel("Camera", scene.camera);
        if (scene.isParticleScene()) {
            ImGui::BulletText("Particle Emitter");
        }
        if (scene.tree != Engine::NullEntity) {
            entityLabel("Tree", scene.tree);
        }
        ImGui::TextDisabled("Entities: %zu", scene.registry.size());
        ImGui::TreePop();
    }

    ImGui::End();
    return clicked;
}

void configureEditorDockLayout() {
    static bool configured = false;
    if (configured) return;

    const ImGuiID dockspaceId = ImGui::GetMainViewport()->ID;
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

    ImGuiID hierarchyId = 0;
    const ImGuiID viewportId = ImGui::DockBuilderSplitNode(
        dockspaceId, ImGuiDir_Left, 0.22f, &hierarchyId, nullptr);
    ImGui::DockBuilderDockWindow("Hierarchy", hierarchyId);
    ImGui::DockBuilderDockWindow("Viewport", viewportId);
    ImGui::DockBuilderFinish(dockspaceId);
    configured = true;
}

void drawEditorMenuBar() {
    static bool showShortcuts = false;
    static bool showAbout = false;

    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("Edit")) {
        // The editor does not yet have a command history, so keep these
        // commands visible and disabled until the corresponding systems exist.
        ImGui::MenuItem("Undo", "Ctrl+Z", false, false);
        ImGui::MenuItem("Redo", "Ctrl+Y", false, false);
        ImGui::Separator();
        ImGui::MenuItem("Cut", "Ctrl+X", false, false);
        ImGui::MenuItem("Copy", "Ctrl+C", false, false);
        ImGui::MenuItem("Paste", "Ctrl+V", false, false);
        ImGui::MenuItem("Duplicate", "Ctrl+D", false, false);
        ImGui::Separator();
        ImGui::MenuItem("Select All", "Ctrl+A", false, false);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Keyboard Shortcuts")) showShortcuts = true;
        ImGui::Separator();
        if (ImGui::MenuItem("About GamEngine Editor")) showAbout = true;
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();

    if (showShortcuts) {
        ImGui::Begin("Keyboard Shortcuts", &showShortcuts);
        ImGui::TextUnformatted("Editor shortcuts");
        ImGui::Separator();
        ImGui::BulletText("WASD + mouse: move and look in Scene View");
        ImGui::BulletText("Ctrl+D: duplicate selected object (coming soon)");
        ImGui::BulletText("Ctrl+Z / Ctrl+Y: undo / redo (coming soon)");
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
}

} // namespace

int main() {
    try {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) throw std::runtime_error(SDL_GetError());
        SDL_Window* window = SDL_CreateWindow("GamEngine Editor - Particles", 1280, 720,
                                              SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
        if (!window) throw std::runtime_error(SDL_GetError());

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
        ImGui::StyleColorsDark();

        Engine::Scene scene(Engine::SceneType::Particles);
        Engine::Renderer renderer;
        renderer.initialize(scene, window);
        Engine::Entity selectedEntity = Engine::NullEntity;
        constexpr auto targetFrame = std::chrono::microseconds{16'667};
        bool running = true;
        while (running) {
            const auto start = std::chrono::steady_clock::now();
            renderer.beginFrame();
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                renderer.processEvent(event);
                if (event.type == SDL_EVENT_QUIT ||
                    (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                     event.window.windowID == SDL_GetWindowID(window))) running = false;
            }
            if (!running) break;

            renderer.beginEditorUiFrame();
            drawEditorMenuBar();
            const ImGuiID dockspaceId = ImGui::GetMainViewport()->ID;
            ImGui::DockSpaceOverViewport(dockspaceId, ImGui::GetMainViewport(),
                                         ImGuiDockNodeFlags_PassthruCentralNode);
            configureEditorDockLayout();
            if (const Engine::Entity clicked = drawHierarchy(scene, selectedEntity);
                clicked != Engine::NullEntity) {
                selectedEntity = clicked;
                renderer.setEditorSelection(selectedEntity);
            }
            const bool sceneCameraInput = drawViewport(
                renderer.gameViewportDescriptor(), renderer.sceneViewportDescriptor());
            renderer.setEditorSceneCameraInput(sceneCameraInput);
            ImGui::Render();
            renderer.renderFrame();

            const auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed < targetFrame) std::this_thread::sleep_for(targetFrame - elapsed);
        }
        renderer.shutdown();
        ImGui::DestroyContext();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Editor error: %s\n", error.what());
        return 1;
    }
}
