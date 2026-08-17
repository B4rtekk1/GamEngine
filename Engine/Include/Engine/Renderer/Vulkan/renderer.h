#pragma once

#include "Engine/Assets/AssetManager.h"
#include "Engine/Renderer/Passes/ForwardPass.h"
#include "Engine/Renderer/Passes/SkyPass.h"
#include "Engine/Renderer/Passes/TonemapPass.h"
#include "Engine/Renderer/Vulkan/graphics_pipeline.h"
#include "Engine/UI/CanvasRenderer.h"

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
struct RenderOptimizationFeatures final {
    bool instancedRendering = true;
    bool meshDeduplication = true;
    bool transformCaching = true;
    bool materialCaching = true;
    bool gpuCulling = true;
    bool occlusionCulling = false;
};

// Owns and runs the Vulkan rendering loop.
class Renderer final {
public:
    explicit Renderer(RenderOptimizationFeatures features = {})
        : optimizationFeatures_(features) {}

    void setOptimizationFeatures(RenderOptimizationFeatures features) noexcept {
        optimizationFeatures_ = features;
    }

    [[nodiscard]] const RenderOptimizationFeatures& optimizationFeatures() const noexcept {
        return optimizationFeatures_;
    }

    /** @brief Runs the renderer using every renderable entity in registry. */
    void run(Scene& scene);

private:
    RenderOptimizationFeatures optimizationFeatures_{};
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
};

} // namespace Engine
