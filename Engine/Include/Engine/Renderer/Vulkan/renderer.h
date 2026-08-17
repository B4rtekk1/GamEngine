#pragma once

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
};

} // namespace Engine
