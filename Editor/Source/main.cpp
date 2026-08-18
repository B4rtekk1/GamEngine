#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "imgui.h"

#include "Engine/Scene/Scene.h"
#include "Engine/ECS/Components/CameraComponent.h"
#include "Engine/Scene/Components/LightComponent.h"
#include "Engine/Renderer/MeshRenderer.h"
#include "Engine/Core/Transform.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <chrono>
#include <cstdio>
#include <thread>
#include <string>

namespace {

const char* entityLabel(const Engine::Scene& scene, const Engine::Entity entity) {
    if (entity == scene.camera) return "Main Camera";
    if (entity == scene.plane) return "Ground Plane";
    if (entity == scene.tree) return "Tree";
    return nullptr;
}

void drawComponentNode(const char* label) {
    ImGui::BulletText("%s", label);
}

void drawEntityNode(const Engine::Scene& scene, const Engine::Entity entity,
                    Engine::Entity& selectedEntity, const char* filter) {
    const char* namedLabel = entityLabel(scene, entity);
    const std::string fallbackLabel = "Entity " + std::to_string(Engine::entityIndex(entity));
    const char* label = namedLabel != nullptr ? namedLabel : fallbackLabel.c_str();

    if (filter[0] != '\0' && std::string(label).find(filter) == std::string::npos) {
        return;
    }

    const bool selected = selectedEntity == entity;
    const bool hasComponents = scene.registry.has<Engine::Transform>(entity) ||
                               scene.registry.has<Engine::MeshRenderer>(entity) ||
                               scene.registry.has<Engine::CameraComponent>(entity) ||
                               scene.registry.has<Engine::LightComponent>(entity);
    const ImGuiTreeNodeFlags flags = (hasComponents ? 0 : ImGuiTreeNodeFlags_Leaf) |
                                     ImGuiTreeNodeFlags_SpanAvailWidth |
                                     (selected ? ImGuiTreeNodeFlags_Selected : 0);

    const bool open = ImGui::TreeNodeEx(
        reinterpret_cast<void*>(static_cast<uintptr_t>(entity)), flags, "%s##%llu",
        label, static_cast<unsigned long long>(entity));
    if (ImGui::IsItemClicked()) selectedEntity = entity;

    if (open) {
        if (scene.registry.has<Engine::Transform>(entity)) drawComponentNode("Transform");
        if (scene.registry.has<Engine::MeshRenderer>(entity)) drawComponentNode("MeshRenderer");
        if (scene.registry.has<Engine::CameraComponent>(entity)) drawComponentNode("Camera");
        if (scene.registry.has<Engine::LightComponent>(entity)) drawComponentNode("Light");
        ImGui::TreePop();
    }
}

void drawSceneView(const Engine::Scene& scene) {
    ImGui::Begin("Scene View");
    ImGui::TextUnformatted("Top view");
    ImGui::SameLine(ImGui::GetWindowWidth() - 92.0f);
    ImGui::TextUnformatted("Target: 60 FPS");

    const ImVec2 canvasMin = ImGui::GetCursorScreenPos();
    const ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    if (canvasSize.x <= 1.0f || canvasSize.y <= 1.0f) {
        ImGui::End();
        return;
    }

    ImGui::InvisibleButton("##SceneCanvas", canvasSize);
    const ImVec2 canvasMax{canvasMin.x + canvasSize.x, canvasMin.y + canvasSize.y};
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(canvasMin, canvasMax, IM_COL32(18, 22, 30, 255));

    constexpr float worldHalfExtent = 25.0f;
    const float scale = std::min(canvasSize.x, canvasSize.y) / (worldHalfExtent * 2.0f);
    const ImVec2 center{canvasMin.x + canvasSize.x * 0.5f,
                        canvasMin.y + canvasSize.y * 0.5f};
    const auto toScreen = [&](const Engine::Vec3& position) {
        return ImVec2{center.x + position.x() * scale,
                      center.y - position.z() * scale};
    };

    for (int grid = -25; grid <= 25; ++grid) {
        const ImU32 color = grid == 0 ? IM_COL32(105, 115, 130, 180)
                                      : IM_COL32(48, 55, 68, 180);
        drawList->AddLine(toScreen(Engine::Vec3{static_cast<float>(grid), 0.0f,
                                                -worldHalfExtent}),
                          toScreen(Engine::Vec3{static_cast<float>(grid), 0.0f,
                                                worldHalfExtent}), color);
        drawList->AddLine(toScreen(Engine::Vec3{-worldHalfExtent, 0.0f,
                                                static_cast<float>(grid)}),
                          toScreen(Engine::Vec3{worldHalfExtent, 0.0f,
                                                static_cast<float>(grid)}), color);
    }

    std::size_t drawnObjects = 0;
    scene.registry.view([&](const Engine::Entity entity) {
        if (!scene.registry.has<Engine::Transform>(entity)) return;
        const auto& transform = scene.registry.get<Engine::Transform>(entity);
        const ImVec2 point = toScreen(transform.position);

        if (scene.registry.has<Engine::MeshRenderer>(entity)) {
            // A dense cube benchmark is represented by a few pixels per object.
            // This keeps the editor viewport responsive while retaining the layout.
            if (drawnObjects++ >= 5000) return;
            const auto& material = scene.registry.get<Engine::MeshRenderer>(entity).material;
            const auto& color = material.baseColor;
            const ImU32 objectColor = IM_COL32(static_cast<int>(color.r() * 255.0f),
                                               static_cast<int>(color.g() * 255.0f),
                                               static_cast<int>(color.b() * 255.0f), 255);
            drawList->AddRectFilled({point.x - 2.0f, point.y - 2.0f},
                                    {point.x + 2.0f, point.y + 2.0f}, objectColor);
        }
        if (entity == scene.camera) {
            drawList->AddCircleFilled(point, 6.0f, IM_COL32(80, 180, 255, 255));
            drawList->AddText({point.x + 8.0f, point.y - 8.0f},
                              IM_COL32(150, 215, 255, 255), "Camera");
        } else if (scene.registry.has<Engine::LightComponent>(entity)) {
            drawList->AddCircleFilled(point, 5.0f, IM_COL32(255, 210, 75, 255));
        }
    });

    drawList->AddRect(canvasMin, canvasMax, IM_COL32(90, 105, 125, 255));
    ImGui::End();
}

void drawEditorUi(const Engine::Scene& scene, bool& showDemoWindow, bool& showStatsWindow) {
    static float position[3] = {0.0f, 0.0f, 0.0f};
    static float rotation[3] = {0.0f, 0.0f, 0.0f};
    static float scale[3] = {1.0f, 1.0f, 1.0f};

    ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(),
                                 ImGuiDockNodeFlags_PassthruCentralNode);

    drawSceneView(scene);

    ImGui::Begin("Editor");
    ImGui::TextUnformatted("GamEngine Editor");
    ImGui::Separator();
    ImGui::TextUnformatted("Minimal SDL3 + Dear ImGui panel");
    ImGui::Checkbox("Show Dear ImGui demo", &showDemoWindow);
    ImGui::Checkbox("Show statistics", &showStatsWindow);
    ImGui::Text("Frame rate: %.1f FPS", ImGui::GetIO().Framerate);
    ImGui::End();

    static Engine::Entity selectedEntity = Engine::NullEntity;
    static char hierarchyFilter[128] = {};

    ImGui::Begin("Scene Hierarchy");
    ImGui::Text("Main Scene (%zu entities)", scene.registry.size());
    ImGui::InputTextWithHint("##HierarchyFilter", "Filter entities...",
                             hierarchyFilter, sizeof(hierarchyFilter));
    ImGui::Separator();
    if (ImGui::TreeNodeEx("Main Scene##root", ImGuiTreeNodeFlags_DefaultOpen |
                                             ImGuiTreeNodeFlags_SpanAvailWidth)) {
        scene.registry.view([&](const Engine::Entity entity) {
            drawEntityNode(scene, entity, selectedEntity, hierarchyFilter);
        });
        ImGui::TreePop();
    }
    ImGui::End();

    ImGui::Begin("Inspector");
    const bool selectedIsValid = scene.registry.valid(selectedEntity);
    ImGui::TextUnformatted(selectedIsValid ? "Selected Entity" : "No entity selected");
    ImGui::Separator();
    if (selectedIsValid && scene.registry.has<Engine::Transform>(selectedEntity)) {
        const auto& transform = scene.registry.get<Engine::Transform>(selectedEntity);
        position[0] = transform.position.x();
        position[1] = transform.position.y();
        position[2] = transform.position.z();
        rotation[0] = transform.rotation.x();
        rotation[1] = transform.rotation.y();
        rotation[2] = transform.rotation.z();
        scale[0] = transform.scale.x();
        scale[1] = transform.scale.y();
        scale[2] = transform.scale.z();
        ImGui::Text("Entity %u", Engine::entityIndex(selectedEntity));
        ImGui::DragFloat3("Position", position, 0.05f);
        ImGui::DragFloat3("Rotation", rotation, 0.5f);
        ImGui::DragFloat3("Scale", scale, 0.01f, 0.01f, 100.0f);
    }
    ImGui::End();

    if (showDemoWindow) ImGui::ShowDemoWindow(&showDemoWindow);
    if (showStatsWindow) {
        ImGui::Begin("Statistics", &showStatsWindow);
        ImGui::Text("SDL renderer: active");
        ImGui::Text("Dear ImGui: %s", IMGUI_VERSION);
        ImGui::End();
    }
}

} // namespace

int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "GamEngine Editor", 1280, 720, SDL_WINDOW_RESIZABLE);
    if (window == nullptr) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (renderer == nullptr) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();

    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    Engine::Scene scene;

    // The editor is intentionally paced independently from the renderer so it
    // does not consume a full CPU core when the scene is idle.
    constexpr auto targetFrameTime = std::chrono::microseconds(16667);
    auto frameStart = std::chrono::steady_clock::now();

    bool running = true;
    bool showDemoWindow = false;
    bool showStatsWindow = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT ||
                (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                 event.window.windowID == SDL_GetWindowID(window))) {
                running = false;
            }
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        drawEditorUi(scene, showDemoWindow, showStatsWindow);
        ImGui::Render();

        SDL_SetRenderDrawColor(renderer, 25, 28, 35, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);

        const auto frameElapsed = std::chrono::steady_clock::now() - frameStart;
        if (frameElapsed < targetFrameTime) {
            std::this_thread::sleep_for(targetFrameTime - frameElapsed);
        }
        frameStart = std::chrono::steady_clock::now();
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
