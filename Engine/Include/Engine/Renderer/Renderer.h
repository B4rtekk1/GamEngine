#pragma once

#include "Engine/Core/Profiler.h"
#include "Engine/Renderer/RenderConfig.h"
#include "Engine/Renderer/Vulkan/MemoryBudgetManager.h"
#include "Engine/ECS/Entity.h"
#include "Engine/Math/Vec3.h"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace Engine {
    class Scene;
    using RenderOptimizationFeatures = RenderFeatures;

    struct EditorUiInitInfo final {
        std::uint64_t instance{};
        std::uint64_t physicalDevice{};
        std::uint64_t device{};
        std::uint64_t queue{};
        std::uint32_t queueFamily{};
        std::uint32_t imageCount{};
        std::uint32_t colorFormat{};
    };

    /** Editor-owned UI backend. Engine only supplies Vulkan resources and draw timing. */
    class EditorUiBackend {
    public:
        virtual ~EditorUiBackend() = default;
        virtual bool initialize(void *window, const EditorUiInitInfo &info) = 0;
        virtual void shutdown() noexcept = 0;
        virtual void processEvent(const void *event) = 0;
        [[nodiscard]] virtual bool wantsTextInput() const = 0;
        virtual void newFrame() = 0;
        [[nodiscard]] virtual std::uintptr_t addTexture(std::uint64_t imageView,
                                                         std::uint32_t imageLayout) = 0;
        virtual void removeTexture(std::uintptr_t texture) noexcept = 0;
        virtual void renderDrawData(std::uint64_t commandBuffer) = 0;
    };

    /** GPU timeline for a fence-completed frame, matched to its CPU frame number. */
    struct GpuProfileFrame final {
        std::uint64_t frameNumber{};
        float frameMilliseconds{};
        std::vector<GpuProfileEvent> events;
    };

    /** Public renderer facade. The concrete graphics backend is an implementation detail. */
    class Renderer final {
    public:
        explicit Renderer(RenderConfig config = {});

        explicit Renderer(RenderOptimizationFeatures features)
            : Renderer(RenderConfig{.features = features}) {
        }

        ~Renderer();

        Renderer(const Renderer &) = delete;

        Renderer &operator=(const Renderer &) = delete;

        Renderer(Renderer &&) = delete;

        Renderer &operator=(Renderer &&) = delete;

        void setOptimizationFeatures(RenderOptimizationFeatures features) noexcept;

        [[nodiscard]] const RenderOptimizationFeatures &optimizationFeatures() const noexcept;

        void setAntialiasingLevel(AntialiasingLevel level) noexcept;

        [[nodiscard]] AntialiasingLevel antialiasingLevel() const noexcept;

        void setShadowQuality(ShadowQuality quality) noexcept;

        [[nodiscard]] ShadowQuality shadowQuality() const noexcept;

        void applyRenderQualityPreset(RenderQualityPreset preset) noexcept;

        void setGtaoQuality(GtaoQuality quality) noexcept;
        [[nodiscard]] GtaoQuality gtaoQuality() const noexcept;
        void setGtaoDebugView(GtaoDebugView view) noexcept;
        [[nodiscard]] GtaoDebugView gtaoDebugView() const noexcept;
        void setPbrDebugView(PbrDebugView view) noexcept;
        [[nodiscard]] PbrDebugView pbrDebugView() const noexcept;
        void setIblQuality(IblQuality quality) noexcept;
        [[nodiscard]] IblQuality iblQuality() const noexcept;

        void setShadowDebugView(ShadowDebugView view) noexcept;

        [[nodiscard]] ShadowDebugView shadowDebugView() const noexcept;

        /** Returns timestamps from the most recently completed GPU frame. */
        [[nodiscard]] std::optional<GpuProfileFrame> gpuProfile() const noexcept;

        /** Latest driver/VMA heap budgets and engine memory-class aggregates. */
        [[nodiscard]] std::vector<GpuMemoryHeapBudget> gpuMemoryHeaps() const;
        [[nodiscard]] std::vector<GpuMemoryCategoryBudget> gpuMemoryCategories() const;
        [[nodiscard]] std::vector<GpuSceneMemoryAllocation> gpuSceneMemory() const;

        /** Sets the project directory used for project-owned runtime caches. Must be called before initializeCore(). */
        void setProjectRoot(std::filesystem::path root);

        void setEditorUiBackend(EditorUiBackend *backend) noexcept;

        // nativeWindow and nativeEvent are opaque platform handles. Applications
        // do not need to include graphics-backend headers to use the renderer.
        /** Initializes presentation and synchronization; editor UI is optional. */
        void initializeCore(Scene &scene, void *nativeWindow);

        /** Creates GPU resources derived from the current scene. */
        void initializeScene(Scene &scene);

        /** Compatibility helper which initializes both stages. */
        void initialize(Scene &scene, void *nativeWindow);

        static void beginFrame();

        [[nodiscard]] EditorEventState pollEditorEvents() const;

        void beginEditorUiFrame() const;

        void processEvent(const void *nativeEvent) const;

        void setEditorSceneCameraInput(bool active) const;

        /** Enables primary game-camera controls for a focused Game View. */
        void setGameCameraInput(bool active) const;

        /** Requests mouse capture after an explicit click in the Game View. */
        void requestGameMouseCapture() const;

        /** Enables the off-screen Scene View render path for the current editor frame. */
        void setSceneViewportActive(bool active) const;

        /** Sets the Scene View render resolution in physical pixels. */
        void setSceneViewportExtent(std::uint32_t width, std::uint32_t height) const;

        /** Updates Scene View navigation before its UI overlays are drawn. */
        void updateEditorSceneCameraInput() const;

        void setEditorSelection(Entity entity) const;

        /** Rebuilds global IBL from an HDR/EXR equirectangular panorama. */
        [[nodiscard]] bool setEnvironmentEquirectangular(const std::filesystem::path& path) const;

        /** Queues a scene-linear six-face capture for a ReflectionProbeComponent. */
        void bakeReflectionProbe(Entity entity) const;

        /** Injects a local disturbance into the virtual-water persistent state cache. */
        void addWaterInteraction(Vec3 worldPosition, float radius = 1.5F, float strength = 1.0F) const;

        void renderFrame() const;

        void synchronizeScene(Scene &scene) const;

        /** Uploads changed vertices for an existing fixed-topology mesh. */
        void updateMeshGeometry(Entity entity, std::uint32_t firstVertex = 0,
                                std::uint32_t vertexCount = std::numeric_limits<std::uint32_t>::max()) const;

        void reloadScene(Scene &scene, void *nativeWindow);

        void reconfigureAntialiasing() const;

        /** Recreates shader-dependent Vulkan pipelines while preserving the window and swapchain. */
        [[nodiscard]] bool reloadShaders() const;

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

        /** Moves the Scene View camera without changing its orientation. */
        void setEditorCameraPosition(Vec3 position) const noexcept;

        void shutdown() noexcept;

    private:
        class Backend;
        class State;
        RenderOptimizationFeatures optimizationFeatures_{};
        AntialiasingLevel antialiasingLevel_ = AntialiasingLevel::Off;
        ShadowQuality shadowQuality_ = ShadowQuality::High;
        GtaoQuality gtaoQuality_ = GtaoQuality::High;
        GtaoDebugView gtaoDebugView_ = GtaoDebugView::Off;
        PbrDebugView pbrDebugView_ = PbrDebugView::FinalLighting;
        IblQuality iblQuality_ = IblQuality::High;
        ShadowDebugView shadowDebugView_ = ShadowDebugView::Off;
        GrassRenderSettings grassSettings_{};
        std::unique_ptr<State> state_;
        std::unique_ptr<Backend> backend_;
        EditorUiBackend *editorUiBackend_ = nullptr;
    };
} // namespace Engine
