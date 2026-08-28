#pragma once

#include "Engine/Renderer/RenderConfig.h"
#include "Engine/ECS/Entity.h"
#include "Engine/Math/Vec3.h"

#include <cstdint>
#include <limits>
#include <memory>

namespace Engine {

class Scene;
using RenderOptimizationFeatures = RenderFeatures;

/** Public renderer facade. The concrete graphics backend is an implementation detail. */
class Renderer final {
public:
    explicit Renderer(RenderConfig config = {});
    explicit Renderer(RenderOptimizationFeatures features)
        : Renderer(RenderConfig{.features = features}) {}
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    void setOptimizationFeatures(RenderOptimizationFeatures features) noexcept;
    [[nodiscard]] const RenderOptimizationFeatures& optimizationFeatures() const noexcept;
    void setAntialiasingLevel(AntialiasingLevel level) noexcept;
    [[nodiscard]] AntialiasingLevel antialiasingLevel() const noexcept;

    // nativeWindow and nativeEvent are opaque platform handles. Applications
    // do not need to include graphics-backend headers to use the renderer.
    void initialize(Scene& scene, void* nativeWindow);

    static void beginFrame();
    [[nodiscard]] EditorEventState pollEditorEvents() const;
    void beginEditorUiFrame() const;
    void processEvent(const void* nativeEvent) const;
    void setEditorSceneCameraInput(bool active) const;
    /** Updates Scene View navigation before its UI overlays are drawn. */
    void updateEditorSceneCameraInput() const;
    void setEditorSelection(Entity entity) const;
    void renderFrame() const;
    void synchronizeScene(Scene& scene) const;
    /** Uploads changed vertices for an existing fixed-topology mesh. */
    void updateMeshGeometry(Entity entity, std::uint32_t firstVertex = 0,
                            std::uint32_t vertexCount = std::numeric_limits<std::uint32_t>::max()) const;
    void reloadScene(Scene& scene, void* nativeWindow);
    void reconfigureAntialiasing() const;
    [[nodiscard]] ViewportHandle gameViewport() const noexcept;
    [[nodiscard]] ViewportHandle sceneViewport() const noexcept;
    /** Current Scene View camera orientation, in degrees. */
    [[nodiscard]] float editorCameraYaw() const noexcept;
    [[nodiscard]] float editorCameraPitch() const noexcept;
    [[nodiscard]] Vec3 editorCameraPosition() const noexcept;
    /** World-space center of an entity's rendered bounds, or its transform position. */
    [[nodiscard]] Vec3 editorGizmoPosition(Entity entity) const noexcept;
    /** Rotates the Scene View camera without changing its position. */
    void setEditorCameraRotation(float yaw, float pitch) const noexcept;
    void shutdown() noexcept;

private:
    class Backend;
    class State;
    RenderOptimizationFeatures optimizationFeatures_{};
    AntialiasingLevel antialiasingLevel_ = AntialiasingLevel::Off;
    std::unique_ptr<State> state_{};
    std::unique_ptr<Backend> backend_{};
};

} // namespace Engine
