#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "imgui.h"

#include <SDL3/SDL.h>

#include <cstdio>

namespace {

void drawEditorUi(bool& showDemoWindow, bool& showStatsWindow) {
    static float position[3] = {0.0f, 0.0f, 0.0f};
    static float rotation[3] = {0.0f, 0.0f, 0.0f};
    static float scale[3] = {1.0f, 1.0f, 1.0f};

    ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(),
                                 ImGuiDockNodeFlags_PassthruCentralNode);

    ImGui::Begin("Editor");
    ImGui::TextUnformatted("GamEngine Editor");
    ImGui::Separator();
    ImGui::TextUnformatted("Minimal SDL3 + Dear ImGui panel");
    ImGui::Checkbox("Show Dear ImGui demo", &showDemoWindow);
    ImGui::Checkbox("Show statistics", &showStatsWindow);
    ImGui::Text("Frame rate: %.1f FPS", ImGui::GetIO().Framerate);
    ImGui::End();

    ImGui::Begin("Scene Hierarchy");
    ImGui::TextUnformatted("Scene");
    if (ImGui::TreeNodeEx("Main Camera", ImGuiTreeNodeFlags_Leaf)) {
        ImGui::TreePop();
    }
    if (ImGui::TreeNodeEx("Directional Light", ImGuiTreeNodeFlags_Leaf)) {
        ImGui::TreePop();
    }
    if (ImGui::TreeNodeEx("Cube", ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Selected)) {
        ImGui::TreePop();
    }
    ImGui::End();

    ImGui::Begin("Inspector");
    ImGui::TextUnformatted("Cube");
    ImGui::Separator();
    ImGui::DragFloat3("Position", position, 0.05f);
    ImGui::DragFloat3("Rotation", rotation, 0.5f);
    ImGui::DragFloat3("Scale", scale, 0.01f, 0.01f, 100.0f);
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
        drawEditorUi(showDemoWindow, showStatsWindow);
        ImGui::Render();

        SDL_SetRenderDrawColor(renderer, 25, 28, 35, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
