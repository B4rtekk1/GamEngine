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

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include "Engine/Renderer/Vulkan/msaa.h"
#include "Engine/Renderer/Vulkan/depth_buffer.h"
#include "Engine/Renderer/Vulkan/hdr_buffer.h"
#include "Engine/Renderer/Vulkan/ViewportRenderTarget.h"
#include "Engine/Renderer/ViewportCamera.h"
#include "Engine/Renderer/shader_loader.h"
#include "Engine/Renderer/Vulkan/buffer.h"
#include "Engine/Renderer/Vulkan/graphics_pipeline.h"
#include "Engine/Renderer/Vulkan/vulkan_device.h"
#include "Engine/Renderer/Vulkan/swapchain.h"
#include "Engine/Renderer/Textures/Texture2D.h"
#include "Engine/Renderer/Geometry/Vertex.h"
#include "Engine/Renderer/Geometry/Mesh.h"
#include "Engine/ECS/Registry.h"
#include "Engine/Scene/Scene.h"
#include "Engine/ECS/Components/CameraComponent.h"
#include "Engine/ECS/Components/ParticleEmitterComponent.h"
#include "Engine/ECS/Components/SmokeEmitterComponent.h"
#include "Engine/ECS/Components/ColorPickerComponent.h"
#include "Engine/ECS/Components/ColliderComponent.h"
#include "Engine/Scene/Components/LightComponent.h"
#include "Engine/Core/Transform.h"
#include "Engine/Core/Camera.h"
#include "Engine/Math/AABB.h"
#include "Engine/Math/Frustum.h"
#include "Engine/Math/Math.h"
#include "Engine/Core/Time.h"
#include "Engine/Renderer/Passes/ForwardPass.h"
#include "Engine/Renderer/Passes/ShadowPass.h"
#include "Engine/Renderer/Passes/SkyPass.h"
#include "Engine/Renderer/Passes/TonemapPass.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Renderer/Culling/CullingTypes.h"
#include "Engine/Renderer/Culling/GPUCullingPass.h"
#include "Engine/Renderer/Culling/IndexedIndirectDrawCount.h"
#include "Engine/Renderer/Culling/HiZBuffer.h"
#include "Engine/Renderer/Culling/HiZPass.h"
#include "Engine/Renderer/Materials/MaterialBuffer.h"
#include "Engine/Renderer/MeshRenderer.h"
#include "Engine/Renderer/Particles/ParticleSystem.h"
#include "Engine/Input/Input.h"
#include "Engine/UI/Canvas.h"
#include "Engine/UI/CanvasRenderer.h"
#include "Engine/UI/PanelElement.h"
#include "Platform/SDL/SDLInput.h"

#include <cstdint>
#include <array>
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

namespace Engine {
    using UniformBufferObject = RendererUniformBufferObject;

    constexpr uint32_t WIDTH = 800;
    constexpr uint32_t HEIGHT = 600;
    constexpr int MAX_FRAMES_IN_FLIGHT = 2;
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
        GraphicsPipeline particlePipeline;
        UI::CanvasRenderer canvasRenderer;
    };

    class Renderer::Backend {
    public:
        explicit Backend(Scene &scene, SDL_Window *window,
                         const RenderOptimizationFeatures &optimizationFeatures,
                         const AntialiasingLevel antialiasingLevel,
                         Assets::AssetManager &assetManager,
                         ForwardPass &forwardPass,
                         SkyPass &skyPass,
                         TonemapPass &tonemapPass,
                         GraphicsPipeline &particlePipeline,
                         UI::CanvasRenderer &canvasRenderer)
            : window(window), forwardPass(forwardPass),
              particlePipeline(particlePipeline),
              skyPass(skyPass),
              tonemapPass(tonemapPass),
              canvasRenderer(canvasRenderer),
              scene(scene),
              registry(scene.registry()),
              optimizationFeatures(optimizationFeatures),
              antialiasingLevel(antialiasingLevel),
              assetManager(assetManager),
              renderables(sceneGpu.renderables),
              instanceBatches(sceneGpu.instanceBatches),
              instanceModels(sceneGpu.instanceModels),
              shadowInstanceModels(sceneGpu.shadowInstanceModels),
              materials(sceneGpu.materials),
              materialSlots(sceneGpu.materialSlots),
              lastTransformRevision(sceneGpu.lastTransformRevision),
              lastMeshRendererRevision(sceneGpu.lastMeshRendererRevision),
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

        void initialize() {
            initWindow();
            initVulkan();
            Time::init();
        }

        static void beginFrame() { Input::beginFrame(); }

        EditorEventState pollEditorEvents() {
            EditorEventState result{};
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (editorUiActive) {
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

        [[nodiscard]] VkDescriptorSet gameViewportTexture() const noexcept { return gameViewportDescriptor; }
        [[nodiscard]] VkDescriptorSet sceneViewportTexture() const noexcept { return sceneViewportDescriptor; }
        [[nodiscard]] float editorCameraYaw() const noexcept { return cameraController.editorYaw(); }
        [[nodiscard]] float editorCameraPitch() const noexcept { return cameraController.editorPitch(); }
        [[nodiscard]] Vec3 editorCameraPosition() const noexcept { return cameraController.editorPosition(); }

        [[nodiscard]] Vec3 editorGizmoPosition(const Entity entity) const noexcept {
            const Registry &readRegistry = registry;
            if (!readRegistry.valid(entity) || !readRegistry.has<Transform>(entity)) {
                return {};
            }

            try {
                const auto &transform = readRegistry.get<Transform>(entity);
                for (const RenderableRecord &record: renderables) {
                    if (record.entity != entity) {
                        continue;
                    }
                    const AABB bounds = record.localBounds.transformed(transform.matrix().native());
                    return Vec3{(bounds.min.native() + bounds.max.native()) * HALF_EXTENT_FACTOR};
                }
                return transform.position;
            } catch (const std::out_of_range &) {
                return {};
            }
        }

        void setEditorCameraRotation(const float yaw, const float pitch) noexcept {
            cameraController.setEditorRotation(yaw, pitch);
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

        void setEditorSelection(const Entity entity) {
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
            Vec3 target = readRegistry.get<Transform>(entity).position;
            float radius = DEFAULT_SELECTION_RADIUS;
            if (editorSelectedRenderable != std::numeric_limits<std::uint32_t>::max()) {
                const RenderableRecord &record = renderables[editorSelectedRenderable];
                const AABB bounds = record.localBounds.transformed(
                    readRegistry.get<Transform>(entity).matrix().native());
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

        // Rebuild only registry-derived GPU data. The instance, device,
        // swapchain and ImGui backend survive ordinary editor scene changes.
        void reloadSceneResources(const Scene &updatedScene) {
            if (&updatedScene != &scene) {
                throw std::invalid_argument("Renderer cannot switch Scene instances while initialized");
            }
            if (device == VK_NULL_HANDLE) {
                return;
            }
            // All scene work is submitted through the graphics queue and each
            // frame has its own fence. Waiting for every in-flight frame is
            // sufficient before destroying scene-owned resources; idling the
            // whole device here needlessly stalls unrelated queue work.
            if (!inFlightFences.empty() && vkWaitForFences(device,
                                                           static_cast<uint32_t>(inFlightFences.size()),
                                                           inFlightFences.data(), VK_TRUE,
                                                           UINT64_MAX) != VK_SUCCESS) {
                throw std::runtime_error("Could not synchronize frames for scene reload");
            }

            destroyCullingResources();
            if (hiZDepthPrepassFramebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device, hiZDepthPrepassFramebuffer, nullptr);
            }
            hiZDepthPrepassFramebuffer = VK_NULL_HANDLE;
            if (hdrFramebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device, hdrFramebuffer, nullptr);
            }
            hdrFramebuffer = VK_NULL_HANDLE;
            destroySceneViewportResources();
            particlePipeline.destroy();
            skyPass.destroy();
            sceneSkyPass.destroy();
            forwardPass.destroy();
            hiZDepthPrepass.destroy();
            shadowPass.destroy();
            sceneDescriptorPass.destroy();
            indexBuffer.destroy();
            vertexBuffer.destroy();
            for (Buffer &buffer: instanceBuffers) {
                buffer.destroy();
            }
            for (Buffer &buffer: shadowInstanceBuffers) {
                buffer.destroy();
            }
            for (Buffer &buffer: materialBuffers) {
                buffer.destroy();
            }
            for (Buffer &buffer: uniformBuffers) {
                buffer.destroy();
            }
            for (Buffer &buffer: sceneUniformBuffers) {
                buffer.destroy();
            }
            for (Texture2D &texture: materialTextures) {
                texture.destroy();
            }
            materialTextures.clear();
            materialTextureDescriptors.clear();
            meshTextureOffsets.clear();
            fallbackMaterialTexture.destroy();
            renderables.clear();
            instanceBatches.clear();
            instanceModels.clear();
            shadowInstanceModels.clear();
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
            createMeshBuffers();
            createInstanceBuffer();
            renderableTopologySignature = currentRenderableTopologySignature();
            createUniformBuffers();
            createSceneUniformBuffers();
            createShadowPass();
            createSceneDescriptorPass();
            createForwardPass();
            createParticleResources();
            createCullingResources();
            createSkyPass();
            createSceneSkyPass();
            createFramebuffers();
            createSceneViewportResources();
            refreshEditorViewportTextures();
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
            const std::uint64_t updatedTopology = currentRenderableTopologySignature();
            if (updatedTopology == renderableTopologySignature) {
                return;
            }
            if (!inFlightFences.empty() && vkWaitForFences(device,
                                                           static_cast<uint32_t>(inFlightFences.size()),
                                                           inFlightFences.data(), VK_TRUE,
                                                           UINT64_MAX) != VK_SUCCESS) {
                throw std::runtime_error("Could not synchronize frames for scene update");
            }

            destroyCullingResources();
            if (hiZDepthPrepassFramebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device, hiZDepthPrepassFramebuffer, nullptr);
                hiZDepthPrepassFramebuffer = VK_NULL_HANDLE;
            }
            if (hdrFramebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device, hdrFramebuffer, nullptr);
                hdrFramebuffer = VK_NULL_HANDLE;
            }
            destroySceneViewportFramebuffer();
            particlePipeline.destroy();
            // A particle emitter may have been removed from the scene.  The
            // compute/render pipelines and the ParticleSystem must share the
            // same lifetime; otherwise the next frame records commands with
            // destroyed pipeline handles.
            particleSystem.reset();
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
            forwardPass.destroy();
            hiZDepthPrepass.destroy();
            shadowPass.destroy();
            sceneDescriptorPass.destroy();
            indexBuffer.destroy();
            vertexBuffer.destroy();
            for (Buffer &buffer: instanceBuffers) { buffer.destroy(); }
            for (Buffer &buffer: shadowInstanceBuffers) { buffer.destroy(); }
            for (Buffer &buffer: materialBuffers) { buffer.destroy(); }
            for (Texture2D &texture: materialTextures) { texture.destroy(); }
            materialTextures.clear();
            materialTextureDescriptors.clear();
            meshTextureOffsets.clear();
            fallbackMaterialTexture.destroy();
            renderables.clear();
            instanceBatches.clear();
            instanceModels.clear();
            shadowInstanceModels.clear();
            materials.clear();
            for (auto &indices: dirtyTransforms) { indices.clear(); }
            for (auto &indices: dirtyMaterials) { indices.clear(); }
            for (auto &indices: dirtyCullingObjects) { indices.clear(); }
            lastTransformRevision = std::numeric_limits<std::uint64_t>::max();
            lastMeshRendererRevision = std::numeric_limits<std::uint64_t>::max();
            hiZValid = false;

            createMaterialTextures();
            createMeshBuffers();
            createInstanceBuffer();
            renderableTopologySignature = currentRenderableTopologySignature();
            createUniformBuffers();
            createSceneUniformBuffers();
            createShadowPass();
            createSceneDescriptorPass();
            createForwardPass();
            createParticleResources();
            createCullingResources();
            createSkyPass();
            createSceneSkyPass();
            createFramebuffers();
            createSceneViewportFramebuffer();
            renderableTopologySignature = updatedTopology;
            assetManager.unload_unused();
        }

        void updateMeshGeometry(const Entity entity) {
            const auto recordIt = sceneGpu.renderableIndices.find(entity);
            if (recordIt == sceneGpu.renderableIndices.end() ||
                !registry.has<MeshRenderer>(entity) || !registry.has<Transform>(entity)) return;
            const auto& renderer = registry.get<MeshRenderer>(entity);
            RenderableRecord& record = renderables[recordIt->second];
            if (!renderer.hasMesh() || renderer.mesh->vertexCount() != record.vertexCount ||
                record.batchIndex >= instanceBatches.size()) {
                synchronizeSceneResources(scene);
                return;
            }
            if (!inFlightFences.empty() && vkWaitForFences(
                    device, static_cast<uint32_t>(inFlightFences.size()), inFlightFences.data(),
                    VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
                throw std::runtime_error("Could not synchronize frames for mesh update");
            }
            vertexBuffer.uploadDeviceLocal(
                renderer.mesh->vertices.data(), sizeof(Vertex) * renderer.mesh->vertices.size(),
                sizeof(Vertex) * record.firstVertex, commandPool,
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

            glm::vec3 sceneMinimum{std::numeric_limits<float>::max()};
            glm::vec3 sceneMaximum{std::numeric_limits<float>::lowest()};
            for (std::size_t batchIndex = 0; batchIndex < instanceBatches.size(); ++batchIndex) {
                bool first = true;
                AABB batchBounds{};
                for (const RenderableRecord& item : renderables) {
                    if (item.batchIndex != batchIndex || !registry.has<Transform>(item.entity)) continue;
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
                if (first) continue;
                instanceBatches[batchIndex].worldBounds = batchBounds;
                sceneMinimum = glm::min(sceneMinimum, batchBounds.min.native());
                sceneMaximum = glm::max(sceneMaximum, batchBounds.max.native());
                if (batchIndex < gpuObjects.size()) {
                    gpuObjects[batchIndex].localAabbMin = {
                        batchBounds.min.x(), batchBounds.min.y(), batchBounds.min.z(), 0.0F};
                    gpuObjects[batchIndex].localAabbMax = {
                        batchBounds.max.x(), batchBounds.max.y(), batchBounds.max.z(), 0.0F};
                }
            }
            if (!gpuObjects.empty()) {
                for (Buffer& buffer : cullingObjectBuffers) {
                    buffer.update(gpuObjects.data(), sizeof(Culling::GPUObjectData) * gpuObjects.size());
                }
                const glm::vec3 center = (sceneMinimum + sceneMaximum) * 0.5F;
                const glm::vec3 halfExtent = (sceneMaximum - sceneMinimum) * 0.5F;
                sceneCenter = Vec3{center};
                sceneRadius = std::max({halfExtent.x, halfExtent.y, halfExtent.z, 1.0F});
            }
            renderableTopologySignature = currentRenderableTopologySignature();
            hiZValid = false;
        }

#include "renderer_backend_state.inl"
#include "renderer_backend_resources.inl"
#include "renderer_backend_frame.inl"
#include "renderer_backend_cleanup.inl"
    };

#include "renderer_api.inl"
} // namespace Engine
