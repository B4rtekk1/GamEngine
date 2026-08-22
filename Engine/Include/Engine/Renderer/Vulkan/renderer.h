#pragma once

#include "Engine/Assets/AssetManager.h"
#include "Engine/Renderer/Passes/ForwardPass.h"
#include "Engine/Renderer/Passes/SkyPass.h"
#include "Engine/Renderer/Passes/TonemapPass.h"
#include "Engine/Renderer/RenderConfig.h"
#include "Engine/Renderer/Vulkan/graphics_pipeline.h"
#include "Engine/UI/CanvasRenderer.h"
#include "Engine/ECS/Entity.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>

namespace Engine {

class Registry;
class Scene;

/**
 * Renderer-wide optimization policy.
 *
 * These switches belong to the engine renderer, not to a particular Scene.
 * A scene only provides ECS data; batching, transform/material caching and
 * GPU culling are renderer features shared by every scene.
 */
using RenderOptimizationFeatures = RenderFeatures;

// Owns and runs the Vulkan rendering loop.
class Renderer final {
public:
    explicit Renderer(RenderConfig config = {});
    explicit Renderer(RenderOptimizationFeatures features)
        : Renderer(RenderConfig{.features = features}) {}

    void setOptimizationFeatures(RenderOptimizationFeatures features) noexcept {
        optimizationFeatures_ = features;
    }

    [[nodiscard]] const RenderOptimizationFeatures& optimizationFeatures() const noexcept {
        return optimizationFeatures_;
    }

    void setAntialiasingLevel(AntialiasingLevel level) noexcept {
        antialiasingLevel_ = level;
    }

    [[nodiscard]] AntialiasingLevel antialiasingLevel() const noexcept {
        return antialiasingLevel_;
    }

    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    /** Initializes graphics resources for an SDL window owned by the application. */
    void initialize(Scene& scene, SDL_Window* window);
    void beginFrame() const;
    /** Polls platform events and forwards them to input and editor UI. */
    [[nodiscard]] EditorEventState pollEditorEvents() const;
    /** Starts the SDL3/Vulkan Dear ImGui frame owned by this renderer. */
    void beginEditorUiFrame() const;
    void processEvent(const SDL_Event& event) const;
    /** Enables mouse/keyboard navigation for the editor's Scene View. */
    void setEditorSceneCameraInput(bool active) const;
    void setEditorSelection(Entity entity) const;
    void renderFrame() const;
    /** Synchronizes changed ECS geometry without recreating the window, swapchain or editor UI. */
    void synchronizeScene(Scene& scene) const;
    /** Rebuilds renderer resources after editor scene geometry changes. */
    void reloadScene(Scene& scene, SDL_Window* window);
    /** Rebuilds only resources affected by the MSAA sample count. */
    void reconfigureAntialiasing() const;
    [[nodiscard]] ViewportHandle gameViewport() const noexcept;
    [[nodiscard]] ViewportHandle sceneViewport() const noexcept;
    void shutdown() noexcept;

private:
    class Backend;
    RenderOptimizationFeatures optimizationFeatures_{};
    AntialiasingLevel antialiasingLevel_ = AntialiasingLevel::Off;
    // Asset cache belongs to the renderer lifetime, not to one Scene.
    Assets::AssetManager assetManager_{};
    // The forward pipeline is renderer infrastructure shared by scenes.
    ForwardPass forwardPass_{};
    // The sky pass is also renderer infrastructure, not scene ECS state.
    SkyPass skyPass_{};
    // Tonemapping presents the renderer output and is shared by scenes.
    TonemapPass tonemapPass_{};
    // The particle pipeline is reusable GPU infrastructure; simulation data is scene-owned.
    GraphicsPipeline particlePipeline_{};
    // UI rendering infrastructure is shared; each scene supplies its Canvas.
    UI::CanvasRenderer canvasRenderer_{};
    std::unique_ptr<Backend> backend_{};
};

} // namespace Engine
