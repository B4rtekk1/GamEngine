#include "imgui.h"

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

void drawViewport(VkDescriptorSet gameDescriptor, VkDescriptorSet sceneDescriptor) {
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
                     imageSize, {0, 1}, {1, 0});
    }
    ImGui::End();
}

} // namespace

int main() {
    try {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) throw std::runtime_error(SDL_GetError());
        SDL_Window* window = SDL_CreateWindow("GamEngine Editor", 1280, 720,
                                              SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
        if (!window) throw std::runtime_error(SDL_GetError());

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
        ImGui::StyleColorsDark();

        Engine::Scene scene;
        Engine::Renderer renderer;
        renderer.initialize(scene, window);
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
            ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
            drawViewport(renderer.gameViewportDescriptor(), renderer.sceneViewportDescriptor());
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
