#include "Engine/Renderer/Vulkan/renderer.h"
#include "Engine/Renderer/Vulkan/renderer_scene_helpers.h"
#include "Engine/Renderer/Vulkan/renderer_types.h"
#include "Engine/Renderer/Vulkan/CameraController.h"
#include "Engine/Renderer/Vulkan/SceneGpuResources.h"

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_vulkan.h"
#include "imgui.h"

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

#include "Engine/Renderer/Vulkan/msaa.h"
#include "Engine/Renderer/Vulkan/depth_buffer.h"
#include "Engine/Renderer/Vulkan/hdr_buffer.h"
#include "Engine/Renderer/Passes/BloomPass.h"
#include "Engine/Renderer/Vulkan/ViewportRenderTarget.h"
#include "Engine/Renderer/ViewportCamera.h"
#include "Engine/Renderer/shader_loader.h"
#include "Engine/Renderer/Vulkan/buffer.h"
#include "Engine/Renderer/Vulkan/upload_context.h"
#include "Engine/Renderer/Vulkan/graphics_pipeline.h"
#include "Engine/Renderer/Vulkan/GpuTimestampProfiler.h"
#include "Engine/Renderer/Vulkan/vulkan_device.h"
#include "Engine/Renderer/Vulkan/swapchain.h"
#include "Engine/Renderer/Textures/Texture2D.h"
#include "Engine/Renderer/Lighting/ImageBasedLighting.h"
#include "Engine/Renderer/Lighting/ReflectionProbeManager.h"
#include "Engine/Renderer/Geometry/GpuVertex.h"
#include "Engine/Renderer/Geometry/Mesh.h"
#include "Engine/ECS/Registry.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/Components/IdentityComponents.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/ECS/Components/CameraComponent.h"
#include "Engine/ECS/Components/ReflectionProbeComponent.h"
#include "Engine/ECS/Components/ParticleEmitterComponent.h"
#include "Engine/ECS/Components/SmokeEmitterComponent.h"
#include "Engine/ECS/Components/ColliderComponent.h"
#include "Engine/ECS/Components/TerrainGrassComponent.h"
#include "Engine/ECS/Components/TerrainComponent.h"
#include "Engine/ECS/Components/WindComponent.h"
#include "Engine/Scene/Components/LightComponent.h"
#include "Engine/Core/Transform.h"
#include "Engine/Core/Camera.h"
#include "Engine/Core/Diagnostics.h"
#include "Engine/Math/AABB.h"
#include "Engine/Math/Frustum.h"
#include "Engine/Math/Math.h"
#include "Engine/Core/Time.h"
#include "Engine/Renderer/Passes/ForwardPass.h"
#include "Engine/Renderer/Passes/ShadowPass.h"
#include "Engine/Renderer/Passes/SkyPass.h"
#include "Engine/Renderer/Passes/TonemapPass.h"
#include "Engine/Renderer/Passes/TemporalAaPass.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Renderer/Culling/CullingTypes.h"
#include "Engine/Renderer/Culling/GPUCullingPass.h"
#include "Engine/Renderer/Culling/GPUInstanceCullingPass.h"
#include "Engine/Renderer/Culling/IndexedIndirectDrawCount.h"
#include "Engine/Renderer/Culling/HiZBuffer.h"
#include "Engine/Renderer/Culling/HiZPass.h"
#include "Engine/Renderer/Culling/MeshletCulling.h"
#include "Engine/Renderer/Materials/MaterialBuffer.h"
#include "Engine/Renderer/MeshRenderer.h"
#include "Engine/Renderer/Particles/ParticleSystem.h"
#include "Engine/Renderer/RenderGraph/RenderGraph.h"
#include "Engine/Input/Input.h"
#include "Engine/UI/Canvas.h"
#include "Engine/UI/CanvasRenderer.h"
#include "Engine/UI/PanelElement.h"
#include "Platform/SDL/SDLInput.h"

#include <cstdint>
#include <array>
#include <bit>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <filesystem>
#include <optional>
#include <memory>
#include <numbers>

namespace Engine {
    using UniformBufferObject = RendererUniformBufferObject;

    struct DirectionalLight final {
        Vec3 direction{};
        Math::Color color{};
        float intensity{};
        bool enabled{};
        bool castShadows{};
    };

    struct WindFrameData final {
        Vec4 directionStrength{};
        Vec4 sourcePositionRange{};
        Vec4 gustFrequencyTime{};
    };

    /** ECS-derived values shared by Game View and Scene View for one frame. */
    struct SceneFrameData final {
        Entity primaryCamera{NullEntity};
        DirectionalLight directionalLight{};
        WindFrameData wind{};
        std::array<LocalLightGPU, MaxLocalLights> lights{};
        std::uint32_t lightCount{};
    };

    struct SceneFrameDataCache final {
        SceneFrameData data{};
        CameraComponent primaryCameraComponent{};
        Vec3 primaryCameraPosition{};
        float primaryCameraYaw{};
        float primaryCameraPitch{};
        bool hasWind{};
        bool initialized{};
        std::uint64_t transformRevision{};
        std::uint64_t lightRevision{};
        std::uint64_t windRevision{};
        std::uint64_t cameraRevision{};
        std::uint64_t uuidRevision{};
        std::uint64_t structuralRevision{};
        std::unordered_map<UUID, Entity> entitiesByUuid;
    };

    constexpr uint32_t WIDTH = 800;
    constexpr uint32_t HEIGHT = 600;
    constexpr int MAX_FRAMES_IN_FLIGHT = 3;
    constexpr float HALF_EXTENT_FACTOR = 0.5F;
    constexpr float DEFAULT_SELECTION_RADIUS = 1.0F;
    constexpr float SCENE_CAMERA_FOV_DEGREES = 60.0F;
    constexpr float SCENE_CAMERA_ASPECT_RATIO = 1.0F;
    constexpr float SCENE_CAMERA_NEAR_CLIP = 0.1F;
    constexpr float SCENE_CAMERA_FAR_CLIP = 1000.0F;
    constexpr float EDITOR_CAMERA_DISTANCE_MULTIPLIER = 3.0F;

#ifdef NDEBUG
    constexpr bool enableValidationLayers = false;
#else
    constexpr bool enableValidationLayers = true;
#endif

    constexpr std::array<const char *, 1> validationLayers = {
        "VK_LAYER_KHRONOS_validation",
    };

    class Renderer::State {
    public:
        Assets::AssetManager assetManager{};
        ForwardPass forwardPass{};
        SkyPass skyPass;
        TonemapPass tonemapPass;
        TemporalAaPass temporalAaPass;
        BloomPass bloomPass;
        GraphicsPipeline particlePipeline;
        UI::CanvasRenderer canvasRenderer;
    };

    class Renderer::Backend {
    public:
        explicit Backend(Scene &scene, SDL_Window *window,
                         const RenderOptimizationFeatures &optimizationFeatures,
                         const AntialiasingLevel antialiasingLevel,
                         const ShadowQuality &shadowQuality,
                         const ShadowDebugView &shadowDebugView,
                         const GrassRenderSettings &grassSettings,
                         Assets::AssetManager &assetManager,
                         ForwardPass &forwardPass,
                         SkyPass &skyPass,
                         TonemapPass &tonemapPass,
                         TemporalAaPass &temporalAaPass,
                         BloomPass &bloomPass,
                         GraphicsPipeline &particlePipeline,
                         UI::CanvasRenderer &canvasRenderer)
            : window(window), forwardPass(forwardPass),
              particlePipeline(particlePipeline),
              skyPass(skyPass),
              tonemapPass(tonemapPass),
              temporalAaPass(temporalAaPass),
              bloomPass(bloomPass),
              canvasRenderer(canvasRenderer),
              scene(scene),
              registry(scene.registry()),
              optimizationFeatures(optimizationFeatures),
              antialiasingLevel(antialiasingLevel),
              shadowQuality(shadowQuality), shadowDebugView(shadowDebugView),
              grassSettings(grassSettings),
              assetManager(assetManager),
              renderables(sceneGpu.renderables),
              instanceBatches(sceneGpu.instanceBatches),
              instanceModels(sceneGpu.instanceModels),
              previousInstanceTransforms(sceneGpu.previousInstanceTransforms),
              materials(sceneGpu.materials),
              materialSlots(sceneGpu.materialSlots),
              lastTransformRevision(sceneGpu.lastTransformRevision),
              lastMeshRendererRevision(sceneGpu.lastMeshRendererRevision),
              lastTerrainGrassRevision(sceneGpu.lastTerrainGrassRevision),
              lastParentRevision(sceneGpu.lastParentRevision),
              dirtyTransforms(sceneGpu.dirtyTransforms),
              dirtyMaterials(sceneGpu.dirtyMaterials),
              dirtyCullingObjects(sceneGpu.dirtyCullingObjects),
              sceneCenter(sceneGpu.sceneCenter),
              sceneRadius(sceneGpu.sceneRadius),
              hasShadowCasters(sceneGpu.hasShadowCasters) {
        }

        ~Backend() {
            cleanup();
        }

        void initializeCore() {
            initWindow();
            initVulkanCore();
            Time::init();
        }

        void initializeSceneResources(const Scene& updatedScene) {
            if (&updatedScene != &scene) {
                throw std::invalid_argument("Renderer cannot switch Scene instances while initialized");
            }
            if (sceneResourcesInitialized) return;
            initSceneResources();
            sceneResourcesInitialized = true;
        }

        [[nodiscard]] bool sceneResourcesReady() const noexcept { return sceneResourcesInitialized; }

        [[nodiscard]] std::optional<GpuProfileFrame> gpuProfile() const noexcept {
            return gpuTimestampProfiler.hasCompletedFrame() ? std::optional{lastGpuProfile} : std::nullopt;
        }

        [[nodiscard]] std::vector<GpuMemoryHeapBudget> gpuMemoryHeaps() const {
            return vulkanDevice.memoryBudgetManager().heaps();
        }

        [[nodiscard]] std::vector<GpuMemoryCategoryBudget> gpuMemoryCategories() const {
            return vulkanDevice.memoryBudgetManager().categories();
        }

        [[nodiscard]] std::vector<GpuSceneMemoryAllocation> gpuSceneMemory() const {
            std::vector<GpuSceneMemoryAllocation> result;
            const auto append = [&](const char* table, const Buffer& buffer) {
                if (buffer.handle() == VK_NULL_HANDLE) return;
                const Buffer::MemoryInfo info = buffer.memoryInfo(vulkanDevice.physical());
                constexpr VkMemoryPropertyFlags deviceLocal = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
                constexpr VkMemoryPropertyFlags hostVisible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
                constexpr VkMemoryPropertyFlags hostCoherent = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
                result.push_back({table, info.heapIndex, (info.properties & deviceLocal) != 0,
                    (info.properties & hostVisible) != 0, (info.properties & hostCoherent) != 0, info.bytes});
            };
            append("Instances", gpuSceneInstanceBuffers[currentFrame]);
            append("Meshes", gpuSceneMeshBuffers[currentFrame]);
            append("Materials", gpuSceneMaterialBuffers[currentFrame]);
            return result;
        }

        static void beginFrame() { Input::beginFrame(); }

        EditorEventState pollEditorEvents() {
            EditorEventState result{};
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                const bool mouseEvent = event.type == SDL_EVENT_MOUSE_MOTION ||
                                        event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                                        event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
                                        event.type == SDL_EVENT_MOUSE_WHEEL;
                // While Play Mode owns the mouse, do not let the hidden cursor
                // activate editor controls below it. Escape releases the capture
                // and restores normal ImGui mouse input on the next frame.
                if (editorUiActive && (!mouseEvent || !cameraController.gameMouseCaptured())) {
                    ImGui_ImplSDL3_ProcessEvent(&event);
                }
                processEvent(event);
                if (event.type == SDL_EVENT_QUIT ||
                    (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                     event.window.windowID == SDL_GetWindowID(window))) {
                    result.quitRequested = true;
                }
                if (event.type == SDL_EVENT_KEY_DOWN && !ImGui::GetIO().WantTextInput) {
                    if (event.key.key == SDLK_F5) {
                        result.togglePlay = true;
                    }
                    if (event.key.key == SDLK_F6) {
                        result.togglePause = true;
                    }
                }
            }
            return result;
        }

        void beginEditorUiFrame() const {
            if (!editorUiActive) {
                throw std::logic_error("Renderer was initialized without an ImGui context");
            }
            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
        }

        [[nodiscard]] VkDescriptorSet gameViewportTexture() const noexcept {
            // Scene View temporarily suppresses the Game View TAA resolve.
            // In that case the temporal history has not been written or
            // transitioned yet, so ImGui must keep sampling the HDR target.
            if (taaResolveActive) {
                const VkDescriptorSet descriptor =
                    gameViewportTemporalDescriptors[temporalAaPass.nextResolvedIndex()];
                if (descriptor != VK_NULL_HANDLE) return descriptor;
            }
            return gameViewportDescriptor;
        }
        [[nodiscard]] VkDescriptorSet sceneViewportTexture() const noexcept { return sceneViewportDescriptor; }
        [[nodiscard]] float editorCameraYaw() const noexcept { return cameraController.editorYaw(); }
        [[nodiscard]] float editorCameraPitch() const noexcept { return cameraController.editorPitch(); }
        [[nodiscard]] Vec3 editorCameraPosition() const noexcept { return cameraController.editorPosition(); }

        [[nodiscard]] glm::mat4 worldModel(const Entity entity) const noexcept {
            const Registry& readRegistry = registry;
            if (!readRegistry.valid(entity) || !readRegistry.has<Transform>(entity)) return glm::mat4{1.0F};
            return readRegistry.get<Transform>(entity).worldMatrix().native();
        }

        [[nodiscard]] Vec3 editorGizmoPosition(const Entity entity) const noexcept {
            const Registry &readRegistry = registry;
            if (!readRegistry.valid(entity) || !readRegistry.has<Transform>(entity)) {
                return {};
            }

            try {
                const glm::mat4 model = worldModel(entity);
                for (const RenderableRecord &record: renderables) {
                    if (record.entity != entity) {
                        continue;
                    }
                    const AABB bounds = record.localBounds.transformed(model);
                    return Vec3{(bounds.min.native() + bounds.max.native()) * HALF_EXTENT_FACTOR};
                }
                return Vec3{glm::vec3{model[3]}};
            } catch (const std::out_of_range &) {
                return {};
            }
        }

        void setEditorCameraRotation(const float yaw, const float pitch) noexcept {
            cameraController.setEditorRotation(yaw, pitch);
            sceneViewportNeedsRender = true;
        }

        void setEditorCameraPosition(const Vec3 position) noexcept {
            cameraController.setEditorPosition(position);
            sceneViewportNeedsRender = true;
        }

        void setSceneViewportActive(const bool active) noexcept {
            const bool wasActive = sceneViewportActive;
            sceneViewportActive = active;
            // Returning to the panel must never show an uninitialized or stale
            // cache after it was hidden while the scene changed.
            if (active && !wasActive) sceneViewportNeedsRender = true;
        }

        void setSceneViewportExtent(const std::uint32_t width, const std::uint32_t height) noexcept {
            if (width != 0 && height != 0) requestedSceneViewportExtent = {width, height};
        }

        void processEvent(const SDL_Event &event) {
            SDLInput::processEvent(event);
            if (event.type == SDL_EVENT_WINDOW_RESIZED ||
                event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                framebufferResized = true;
            }
        }

        void setEditorSceneCameraInput(const bool active) {
            cameraController.setEditorInputEnabled(active);
        }

        void setGameCameraInput(const bool active) {
            cameraController.setGameInputEnabled(active);
        }

        void requestGameMouseCapture() {
            cameraController.requestGameMouseCapture();
        }

        void setEditorSelection(const Entity entity) {
            sceneViewportNeedsRender = true;
            editorSelectedEntity = entity;
            editorSelectedRenderable = std::numeric_limits<std::uint32_t>::max();
            for (std::size_t index = 0; index < renderables.size(); ++index) {
                if (renderables[index].entity == entity) {
                    editorSelectedRenderable = static_cast<std::uint32_t>(index);
                    break;
                }
            }

            // Selection is a read-only editor operation. Use a const view of
            // the registry so Registry::get() does not advance the scene
            // mutation revision and trigger a full renderer reload.
            const Registry &readRegistry = registry;
            if (!readRegistry.has<Transform>(entity)) { return; }
            Vec3 target = Vec3{glm::vec3{worldModel(entity)[3]}};
            float radius = DEFAULT_SELECTION_RADIUS;
            if (editorSelectedRenderable != std::numeric_limits<std::uint32_t>::max()) {
                const RenderableRecord &record = renderables[editorSelectedRenderable];
                const AABB bounds = record.localBounds.transformed(
                    worldModel(entity));
                target = Vec3{(bounds.min.native() + bounds.max.native()) * HALF_EXTENT_FACTOR};
                radius = std::max(glm::length(bounds.max.native() - bounds.min.native()) * HALF_EXTENT_FACTOR,
                                  DEFAULT_SELECTION_RADIUS);
            }

            Camera sceneCamera{
                Degrees{SCENE_CAMERA_FOV_DEGREES}, SCENE_CAMERA_ASPECT_RATIO,
                SCENE_CAMERA_NEAR_CLIP, SCENE_CAMERA_FAR_CLIP
            };
            sceneCamera.setPosition(cameraController.editorPosition());
            sceneCamera.setRotation(Degrees{cameraController.editorYaw()},
                                    Degrees{cameraController.editorPitch()});
            cameraController.setEditorPosition(
                target - sceneCamera.forward() * (radius * EDITOR_CAMERA_DISTANCE_MULTIPLIER));
        }

        void renderFrame() {
            if (!sceneResourcesInitialized) {
                Time::update();
                drawCoreFrame();
                return;
            }
            TransformSystem::update(registry);
            Time::update();
            // Scene View navigation is updated before the editor UI so its
            // rendered image and gizmo overlay use the same camera state.
            // The game camera remains a render-frame concern.
            if (!cameraController.editorInputEnabled()) {
                updateCameraInput();
            }
            updateFpsCounter();
            drawFrame();
        }

        void waitIdle() const {
            if (device != VK_NULL_HANDLE) {
                vkDeviceWaitIdle(device);
            }
        }

        // Descriptor pools and scene-wide buffers are shared by every frame
        // slot.  Waiting only for currentFrame is therefore insufficient
        // before a Play/Stop snapshot or another global scene rebuild.
        void waitForAllFrames() const {
            if (device == VK_NULL_HANDLE || inFlightFences.empty()) {
                return;
            }
            if (vkWaitForFences(device, static_cast<std::uint32_t>(inFlightFences.size()),
                                inFlightFences.data(), VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
                throw std::runtime_error("Could not synchronize all frames");
            }
        }

        [[nodiscard]] bool setEnvironmentEquirectangular(const std::filesystem::path& path) {
            if (!sceneResourcesInitialized || device == VK_NULL_HANDLE || path.empty()) return false;
            try {
                waitIdle();
                ImageBasedLighting replacement;
                replacement.create(vulkanDevice.physical(), device, commandPool,
                                   vulkanDevice.graphicsQueue(), vulkanDevice.allocator(), path);
                imageBasedLighting.swap(replacement);
                const auto descriptors = imageBasedLighting.descriptors();
                shadowPass.updateImageBasedLightingDescriptors(descriptors);
                sceneDescriptorPass.updateImageBasedLightingDescriptors(descriptors);
                const auto environment = imageBasedLighting.environmentDescriptor();
                skyPass.setEnvironment(environment);
                sceneSkyPass.setEnvironment(environment);
                environmentEquirectangularPath = path;
                sceneViewportNeedsRender = true;
                return true;
            } catch (const std::exception& exception) {
                std::cerr << "[Renderer] Could not rebuild environment IBL: " << exception.what() << '\n';
                return false;
            }
        }

        void bakeReflectionProbe(const Entity entity) {
            if (sceneResourcesInitialized) reflectionProbeManager.bakeProbe(entity);
        }

        [[nodiscard]] bool reloadShaders() {
            if (device == VK_NULL_HANDLE || !sceneResourcesInitialized) return false;

            // Validate every newly copied module before touching a live
            // pipeline.  A malformed SPIR-V file (for example an interrupted
            // copy) therefore leaves the complete active renderer intact.
            try {
                const auto shaderDirectory = assetManager.asset_root() / "shaders";
                for (const auto& entry : std::filesystem::directory_iterator(shaderDirectory)) {
                    if (entry.is_regular_file() && entry.path().extension() == ".spv") {
                        static_cast<void>(Vkutil::loadShaderModule(device, assetManager, entry.path()));
                    }
                }
            } catch (const std::exception& exception) {
                std::cerr << "[Renderer] Shader validation failed: " << exception.what() << '\n';
                return false;
            }

            // A hot reload is an infrequent editor operation.  Waiting here
            // makes every old pipeline safe to release, while deliberately
            // retaining the Vulkan instance, swapchain, ImGui backend, scene
            // geometry and viewport images.
            vkDeviceWaitIdle(device);
            try {
                tonemapPass.destroy();
                temporalAaPass.destroy();
                bloomPass.destroy();
                destroyVelocityResources();
                if (hdrFramebuffer != VK_NULL_HANDLE) {
                    vkDestroyFramebuffer(device, hdrFramebuffer, nullptr);
                    hdrFramebuffer = VK_NULL_HANDLE;
                }
                destroySceneViewportFramebuffer();

                canvasRenderer.destroy();
                particlePipeline.destroy();
                if (particleComputePipeline != VK_NULL_HANDLE) {
                    vkDestroyPipeline(device, particleComputePipeline, nullptr);
                    particleComputePipeline = VK_NULL_HANDLE;
                }
                if (particleComputePipelineLayout != VK_NULL_HANDLE) {
                    vkDestroyPipelineLayout(device, particleComputePipelineLayout, nullptr);
                    particleComputePipelineLayout = VK_NULL_HANDLE;
                }
                skyPass.destroy();
                sceneSkyPass.destroy();
                sceneViewportForwardPass.destroy();
                forwardPass.destroy();
                shadowPass.destroy();
                sceneDescriptorPass.destroy();
                destroyCullingResources();

                createCullingResources();
                createShadowPass();
                createSceneDescriptorPass();
                createForwardPass();
                createSceneViewportForwardPass();
                createParticleResources();
                createSkyPass();
                createSceneSkyPass();
                createFramebuffers();
                createSceneViewportFramebuffer();
                createTemporalAaPass();
                createBloomPass();
                createTonemapPass();
                createUIResources();
                refreshEditorViewportTextures();
                sceneViewportCacheValid = false;
                sceneViewportImageInitialized = false;
                sceneViewportNeedsRender = true;
                assetManager.unload_unused();
                return true;
            } catch (const std::exception& exception) {
                std::cerr << "[Renderer] Shader reload failed: " << exception.what() << '\n';
                return false;
            }
        }

        // Rebuild only registry-derived GPU data. The instance, device,
        // swapchain and ImGui backend survive ordinary editor scene changes.
        void reloadSceneResources(const Scene &updatedScene) {
            if (&updatedScene != &scene) {
                throw std::invalid_argument("Renderer cannot switch Scene instances while initialized");
            }
            if (device == VK_NULL_HANDLE) {
                return;
            }
            // A scene snapshot replaces descriptor sets and buffers consumed
            // by all graphics frame slots. Retire those submissions before
            // releasing the shared resources, without stalling unrelated GPU
            // work with vkDeviceWaitIdle().
            // Retire the graphics submissions associated with every frame
            // slot before any descriptor pool or shared buffer is destroyed.
            waitForAllFrames();

            destroyCullingResources();
            tonemapPass.destroy();
            temporalAaPass.destroy();
            bloomPass.destroy();
            destroyVelocityResources();
            if (hdrFramebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device, hdrFramebuffer, nullptr);
            }
            hdrFramebuffer = VK_NULL_HANDLE;
            destroySceneViewportResources();
            particlePipeline.destroy();
            skyPass.destroy();
            sceneSkyPass.destroy();
            forwardPass.destroy();
            shadowPass.destroy();
            sceneDescriptorPass.destroy();
            indexBuffer.destroy();
            vertexBuffer.destroy();
            geometryHeapAllocations.clear();
            geometryHeapVertexHighWater = 0;
            geometryHeapIndexHighWater = 0;
            sceneGpu.database.clear();
            for (Buffer &buffer: instanceBuffers) {
                buffer.destroy();
            }
            for (Buffer &buffer: materialBuffers) {
                buffer.destroy();
            }
            for (Buffer& buffer : gpuSceneInstanceBuffers) buffer.destroy();
            for (Buffer& buffer : gpuSceneMeshBuffers) buffer.destroy();
            for (Buffer& buffer : gpuSceneMaterialBuffers) buffer.destroy();
            for (Buffer& buffer : visibleInstanceBuffers) buffer.destroy();
            for (Buffer& buffer : visibleInstanceCountBuffers) buffer.destroy();
            for (Buffer &buffer: uniformBuffers) {
                buffer.destroy();
            }
            for (Texture2D &texture: materialTextures) {
                texture.destroy();
            }
            materialTextures.clear();
            materialTextureDescriptors.clear();
            meshTextureOffsets.clear();
            fallbackMaterialTexture.destroy();
            imageBasedLighting.destroy();
            renderables.clear();
            instanceBatches.clear();
            instanceModels.clear();
            materials.clear();
            for (auto &indices: dirtyTransforms) {
                indices.clear();
            }
            for (auto &indices: dirtyMaterials) {
                indices.clear();
            }
            for (auto &indices: dirtyCullingObjects) {
                indices.clear();
            }
            lastTransformRevision = std::numeric_limits<std::uint64_t>::max();
            lastMeshRendererRevision = std::numeric_limits<std::uint64_t>::max();
            lastTerrainGrassRevision = std::numeric_limits<std::uint64_t>::max();
            lastParentRevision = std::numeric_limits<std::uint64_t>::max();
            hiZValid = false;

            // The previous scene may have owned a particle system. Its GPU
            // resources must not survive a registry replacement into a scene
            // without a ParticleEmitterComponent.
            particleSystem.reset();
            if (particleComputePipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device, particleComputePipeline, nullptr);
                particleComputePipeline = VK_NULL_HANDLE;
            }
            if (particleComputePipelineLayout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device, particleComputePipelineLayout, nullptr);
                particleComputePipelineLayout = VK_NULL_HANDLE;
            }

            createMaterialTextures();
            createImageBasedLighting();
            createMeshBuffers();
            createInstanceBuffer();
            lastRenderTopologyRevision = registry.renderTopologyRevision();
            lastParticleEmitterRevision = registry.componentRevision<ParticleEmitterComponent>();
            lastSmokeEmitterRevision = registry.componentRevision<SmokeEmitterComponent>();
            createUniformBuffers();
            createSceneUniformBuffers();
            createCullingResources();
            createShadowPass();
            createSceneDescriptorPass();
            createForwardPass();
            createParticleResources();
            createSkyPass();
            createFramebuffers();
            createSceneViewportResources();
            createSceneSkyPass();
            createTemporalAaPass();
            createBloomPass();
            createTonemapPass();
            refreshEditorViewportTextures();
            sceneViewportCacheValid = false;
            sceneViewportImageInitialized = false;
            sceneViewportNeedsRender = true;
            assetManager.unload_unused();
        }

        // Recreate only data derived from renderable ECS components.  In
        // particular, preserve the Vulkan instance/device, swapchain, ImGui
        // backend and Scene View images: adding an object must not look like a
        // complete scene reload to the editor.
        void synchronizeSceneResources(const Scene &updatedScene) {
            if (&updatedScene != &scene) {
                throw std::invalid_argument("Renderer cannot switch Scene instances while initialized");
            }
            if (device == VK_NULL_HANDLE) {
                return;
            }
            // The editor reports every ECS structural change here, including
            // hierarchy, scripts and colliders. Avoid stalling the GPU and
            // recreating render resources unless the renderable topology
            // itself changed.
            const std::uint64_t updatedTopologyRevision = registry.renderTopologyRevision();
            const std::uint64_t updatedParticleEmitterRevision =
                registry.componentRevision<ParticleEmitterComponent>();
            const std::uint64_t updatedSmokeEmitterRevision =
                registry.componentRevision<SmokeEmitterComponent>();
            if (updatedTopologyRevision == lastRenderTopologyRevision &&
                updatedParticleEmitterRevision == lastParticleEmitterRevision &&
                updatedSmokeEmitterRevision == lastSmokeEmitterRevision) {
                return;
            }
            sceneViewportNeedsRender = true;
            waitForAllFrames();
            // Buffer destruction retires its own upload fence.  In-flight
            // rendering is covered by the per-frame fences above; a queue
            // wide idle would turn every hierarchy edit into a GPU hitch.

            // ECS topology is not renderer topology.  Only resources whose
            // contents or descriptor bindings refer to the renderable tables
            // are rebuilt below.  In particular, the render targets and all
            // post processing passes deliberately survive this operation:
            // adding an entity must not discard TAA history or recreate the
            // Bloom/Tonemap/sky/forward/particle pipelines. ShadowPass now
            // updates descriptor-set contents in place, preserving its set
            // layout and therefore every pipeline which consumes it.
            // Geometry is an append-only heap. Topology changes only append
            // sub-allocations; old mesh ranges stay valid for in-flight draw
            // calls. createMeshBuffers() grows its backing buffer only when
            // this heap has exhausted its reserved capacity.
            // Persistent scene tables retain their backing allocations. Their
            // creators grow geometrically only when a new delta exceeds the
            // current capacity, otherwise they overwrite changed records.
            for (Texture2D &texture: materialTextures) { texture.destroy(); }
            materialTextures.clear();
            materialTextureDescriptors.clear();
            meshTextureOffsets.clear();
            fallbackMaterialTexture.destroy();
            renderables.clear();
            instanceBatches.clear();
            instanceModels.clear();
            materials.clear();
            for (auto &indices: dirtyTransforms) { indices.clear(); }
            for (auto &indices: dirtyMaterials) { indices.clear(); }
            for (auto &indices: dirtyCullingObjects) { indices.clear(); }
            lastTransformRevision = std::numeric_limits<std::uint64_t>::max();
            lastMeshRendererRevision = std::numeric_limits<std::uint64_t>::max();
            lastTerrainGrassRevision = std::numeric_limits<std::uint64_t>::max();
            lastParentRevision = std::numeric_limits<std::uint64_t>::max();
            hiZValid = false;

            createMaterialTextures();
            createMeshBuffers();
            createInstanceBuffer();
            lastRenderTopologyRevision = registry.renderTopologyRevision();
            lastParticleEmitterRevision = registry.componentRevision<ParticleEmitterComponent>();
            lastSmokeEmitterRevision = registry.componentRevision<SmokeEmitterComponent>();
            // Camera UBOs are independent of renderable topology. Recreating
            // them here would destroy buffers still referenced by the sky
            // descriptor sets, while the sky passes intentionally survive a
            // topology-only rebuild.
            if (canReuseCullingResources()) {
                // Descriptor sets and pipelines still point at the same
                // capacity-reserved buffers. Only batch records changed.
                refreshCullingBatchObjects();
            } else {
                destroyCullingResources();
                createCullingResources();
            }
            createShadowPass();
            createSceneDescriptorPass();
            lastRenderTopologyRevision = updatedTopologyRevision;
            lastParticleEmitterRevision = updatedParticleEmitterRevision;
            lastSmokeEmitterRevision = updatedSmokeEmitterRevision;
            assetManager.unload_unused();
        }

        void updateMeshGeometry(const Entity entity, const std::uint32_t firstVertex,
                                const std::uint32_t requestedVertexCount) {
            const auto recordIt = sceneGpu.renderableIndices.find(entity);
            if (recordIt == sceneGpu.renderableIndices.end() ||
                !registry.has<MeshRenderer>(entity) || !registry.has<Transform>(entity)) return;
            const auto& renderer = registry.get<MeshRenderer>(entity);
            RenderableRecord& record = renderables[recordIt->second];
            // Imported runtime assets release decoded vertices after upload.
            // A geometry edit therefore has to go through the editor/source
            // path, which pins data and rebuilds the GPU resource first.
            if (!renderer.hasMesh() || !renderer.mesh.hasSourceData() ||
                renderer.mesh->vertexCount() != record.vertexCount ||
                record.batchIndex >= instanceBatches.size()) {
                synchronizeSceneResources(scene);
                return;
            }
            if (firstVertex >= renderer.mesh->vertexCount()) return;
            const std::uint32_t vertexCount = std::min(
                requestedVertexCount, renderer.mesh->vertexCount() - firstVertex);
            if (vertexCount == 0) return;
            // uploadDeviceLocal submits the copy after all graphics work already
            // queued on this queue. Avoid waiting for every frame in flight here:
            // that global stall made terrain sculpting block on unrelated frames.
            std::vector<GpuVertex> packedVertices;
            packedVertices.reserve(vertexCount);
            for (std::uint32_t index = 0; index < vertexCount; ++index) {
                packedVertices.push_back(GpuVertex::pack(renderer.mesh->vertices[firstVertex + index]));
            }
            vertexBuffer.uploadDeviceLocal(
                packedVertices.data(), sizeof(GpuVertex) * packedVertices.size(),
                sizeof(GpuVertex) * (record.firstVertex + firstVertex), commandPool,
                vulkanDevice.graphicsQueue());

            AABB localBounds{
                .min = Vec3{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                            std::numeric_limits<float>::max()},
                .max = Vec3{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                            std::numeric_limits<float>::lowest()},
            };
            for (const Vertex& vertex : renderer.mesh->vertices) {
                localBounds.min.setX(std::min(localBounds.min.x(), vertex.position.x()));
                localBounds.min.setY(std::min(localBounds.min.y(), vertex.position.y()));
                localBounds.min.setZ(std::min(localBounds.min.z(), vertex.position.z()));
                localBounds.max.setX(std::max(localBounds.max.x(), vertex.position.x()));
                localBounds.max.setY(std::max(localBounds.max.y(), vertex.position.y()));
                localBounds.max.setZ(std::max(localBounds.max.z(), vertex.position.z()));
            }
            record.localBounds = localBounds;
            InstanceBatch& changedBatch = instanceBatches[record.batchIndex];
            changedBatch.mesh = renderer.mesh.get();

            bool first = true;
            AABB batchBounds{};
            for (const std::size_t index : sceneGpu.batchRenderableIndices[record.batchIndex]) {
                const RenderableRecord& item = renderables[index];
                if (!registry.has<Transform>(item.entity)) continue;
                const AABB world = item.localBounds.transformed(
                    registry.get<Transform>(item.entity).matrix().native());
                if (first) {
                    batchBounds = world;
                    first = false;
                } else {
                    batchBounds.min = Vec3{glm::min(batchBounds.min.native(), world.min.native())};
                    batchBounds.max = Vec3{glm::max(batchBounds.max.native(), world.max.native())};
                }
            }
            if (!first && record.batchIndex < gpuObjects.size()) {
                changedBatch.worldBounds = batchBounds;
                auto& gpuObject = gpuObjects[record.batchIndex];
                gpuObject.localAabbMin = {batchBounds.min.x(), batchBounds.min.y(), batchBounds.min.z(), 0.0F};
                gpuObject.localAabbMax = {batchBounds.max.x(), batchBounds.max.y(), batchBounds.max.z(), 0.0F};
                for (Buffer& buffer : cullingObjectBuffers) {
                    buffer.update(&gpuObject, sizeof(gpuObject),
                                  sizeof(Culling::GPUObjectData) * record.batchIndex);
                }

                glm::vec3 sceneMinimum{std::numeric_limits<float>::max()};
                glm::vec3 sceneMaximum{std::numeric_limits<float>::lowest()};
                for (const InstanceBatch& batch : instanceBatches) {
                    sceneMinimum = glm::min(sceneMinimum, batch.worldBounds.min.native());
                    sceneMaximum = glm::max(sceneMaximum, batch.worldBounds.max.native());
                }
                const glm::vec3 center = (sceneMinimum + sceneMaximum) * 0.5F;
                const glm::vec3 halfExtent = (sceneMaximum - sceneMinimum) * 0.5F;
                sceneCenter = Vec3{center};
                sceneRadius = std::max({halfExtent.x, halfExtent.y, halfExtent.z, 1.0F});
            }
            lastRenderTopologyRevision = registry.renderTopologyRevision();
            lastParticleEmitterRevision = registry.componentRevision<ParticleEmitterComponent>();
            lastSmokeEmitterRevision = registry.componentRevision<SmokeEmitterComponent>();
            hiZValid = false;
            // Vertex edits can change every depth sample inside the previous
            // and current bounds. They are interactive and infrequent, so a
            // full shadow refresh is preferable to retaining stale terrain.
            if (changedBatch.castShadow) {
                shadowPass.invalidateCache();
                sceneDescriptorPass.invalidateCache();
            }
        }

#include "renderer_backend_state.inl"
#include "renderer_backend_resources.inl"
#include "renderer_backend_frame.inl"
#include "renderer_backend_cleanup.inl"
    };

#include "renderer_api.inl"
} // namespace Engine
