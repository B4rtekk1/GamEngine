#include "Engine/Renderer/Vulkan/renderer.h"

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

constexpr uint32_t WIDTH = 800;
constexpr uint32_t HEIGHT = 600;
constexpr int MAX_FRAMES_IN_FLIGHT = 2;

namespace {
    struct UniformBufferObject {
        Mat4 view;
        Mat4 projection;
        Mat4 lightSpace;
        glm::vec4 cameraPosition;
        glm::vec4 lightDirectionIntensity;
        glm::vec4 lightColor;
        std::uint32_t shadowEnabled{0};
        std::uint32_t materialSlots{1};
        std::uint32_t selectedInstance{std::numeric_limits<std::uint32_t>::max()};
        std::uint32_t materialSlotsPadding{};
    };
}

#ifdef NDEBUG
constexpr bool enableValidationLayers = false;
#else
constexpr bool enableValidationLayers = true;
#endif

const std::vector<const char*> validationLayers = {
    "VK_LAYER_KHRONOS_validation"
};

class Renderer::Backend {
    public:
        explicit Backend(Scene& scene, SDL_Window* window,
                           const RenderOptimizationFeatures& optimizationFeatures,
                           const AntialiasingLevel antialiasingLevel,
                           Assets::AssetManager& assetManager,
                           ForwardPass& forwardPass,
                           SkyPass& skyPass,
                           TonemapPass& tonemapPass,
                           GraphicsPipeline& particlePipeline,
                           UI::CanvasRenderer& canvasRenderer)
            : window(window), scene(scene),
              registry(scene.registry),
              optimizationFeatures(optimizationFeatures),
              antialiasingLevel(antialiasingLevel),
              assetManager(assetManager),
              forwardPass(forwardPass),
              skyPass(skyPass),
              tonemapPass(tonemapPass),
              particlePipeline(particlePipeline),
              canvasRenderer(canvasRenderer) {}

        ~Backend() {
            cleanup();
        }

        void initialize() {
            initWindow();
            initVulkan();
            Time::init();
        }

        void beginFrame() { Input::beginFrame(); }

        void beginEditorUiFrame() {
            if (!editorUiActive) throw std::logic_error("Renderer was initialized without an ImGui context");
            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
        }

        [[nodiscard]] VkDescriptorSet gameViewportTexture() const noexcept { return gameViewportDescriptor; }
        [[nodiscard]] VkDescriptorSet sceneViewportTexture() const noexcept { return sceneViewportDescriptor; }

        void processEvent(const SDL_Event& event) {
            SDLInput::processEvent(event);
            if (event.type == SDL_EVENT_WINDOW_RESIZED ||
                event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                framebufferResized = true;
            }
        }

        void setEditorSceneCameraInput(const bool active) {
            editorSceneCameraInput = active;
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
            const Registry& readRegistry = registry;
            if (!readRegistry.has<Transform>(entity)) return;
            Vec3 target = readRegistry.get<Transform>(entity).position;
            float radius = 1.0f;
            if (editorSelectedRenderable != std::numeric_limits<std::uint32_t>::max()) {
                const RenderableRecord& record = renderables[editorSelectedRenderable];
                const AABB bounds = record.localBounds.transformed(
                    readRegistry.get<Transform>(entity).matrix().native());
                target = Vec3{(bounds.min.native() + bounds.max.native()) * 0.5f};
                radius = std::max(glm::length(bounds.max.native() - bounds.min.native()) * 0.5f,
                                  1.0f);
            }

            Camera sceneCamera{Degrees{60.0f}, 1.0f, 0.1f, 1000.0f};
            sceneCamera.setPosition(editorSceneCameraPosition);
            sceneCamera.setRotation(Degrees{editorSceneCameraYaw},
                                    Degrees{editorSceneCameraPitch});
            editorSceneCameraPosition = target - sceneCamera.forward() * (radius * 3.0f);
        }

        void renderFrame() {
            Time::update();
            updateCameraInput();
            updateFpsCounter();
            drawFrame();
        }

        void waitIdle() const {
            if (device != VK_NULL_HANDLE) {
                vkDeviceWaitIdle(device);
            }
        }

    private:
        friend class Renderer;
        SDL_Window* window = nullptr;

        VkInstance instance{};
        VkDebugUtilsMessengerEXT debugMessenger{};
        VkSurfaceKHR surface{};

        VulkanDevice vulkanDevice;
        VkDevice device = VK_NULL_HANDLE;

        Swapchain swapchain;
        VkFramebuffer hdrFramebuffer = VK_NULL_HANDLE;
        // The editor's Scene View uses this actual render output rather than a
        // UI-only placeholder. It has the same attachment formats as the game
        // path, so both views share the forward/sky/particle pipelines.
        ViewportRenderTarget sceneViewportTarget;
        VkFramebuffer sceneViewportFramebuffer = VK_NULL_HANDLE;
        VkRenderPass editorUiRenderPass = VK_NULL_HANDLE;
        std::vector<VkFramebuffer> editorUiFramebuffers;
        VkDescriptorSet gameViewportDescriptor = VK_NULL_HANDLE;
        VkDescriptorSet sceneViewportDescriptor = VK_NULL_HANDLE;
        bool editorUiActive = false;

        MsaaResources msaa;
        HdrBuffer hdrBuffer;
        // A single-sample depth target used exclusively to generate Hi-Z when
        // the visible geometry is rendered with MSAA.
        DepthBuffer hiZDepthBuffer;
        ForwardPass hiZDepthPrepass;
        VkFramebuffer hiZDepthPrepassFramebuffer = VK_NULL_HANDLE;

        ForwardPass& forwardPass;
        GraphicsPipeline& particlePipeline;
        std::unique_ptr<Particles::ParticleSystem> particleSystem;
        VkPipelineLayout particleComputePipelineLayout = VK_NULL_HANDLE;
        VkPipeline particleComputePipeline = VK_NULL_HANDLE;
        SkyPass& skyPass;
        TonemapPass& tonemapPass;
        UI::CanvasRenderer& canvasRenderer;
        Texture2D fpsFontTexture;
        Texture2D fallbackMaterialTexture;
        std::vector<Texture2D> materialTextures;
        std::vector<VkDescriptorImageInfo> materialTextureDescriptors;
        std::unordered_map<const Mesh*, std::uint32_t> meshTextureOffsets;
        DepthBuffer depthBuffer;
        ShadowPass shadowPass;
        // A descriptor-compatible pass for Scene View. It owns an independent
        // per-frame camera UBO while reusing the exact forward material layout.
        ShadowPass sceneDescriptorPass;
        SkyPass sceneSkyPass;
        Scene& scene;
        Registry& registry;
        const RenderOptimizationFeatures& optimizationFeatures;
        AntialiasingLevel antialiasingLevel;
        Assets::AssetManager& assetManager;
        std::optional<Camera> camera;
        Buffer vertexBuffer;
        Buffer indexBuffer;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> instanceBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowInstanceBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> materialBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> uniformBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> sceneUniformBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> cullingObjectBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> cullingUniformBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowCullingUniformBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> indirectBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowIndirectBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> drawCountBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowDrawCountBuffers;
        std::array<Culling::GPUCullingPass, MAX_FRAMES_IN_FLIGHT> gpuCullingPasses;
        std::array<Culling::GPUCullingPass, MAX_FRAMES_IN_FLIGHT> shadowCullingPasses;
        std::array<Culling::IndexedIndirectDrawCount, MAX_FRAMES_IN_FLIGHT> indirectDraws;
        std::array<Culling::IndexedIndirectDrawCount, MAX_FRAMES_IN_FLIGHT> shadowIndirectDraws;
        Culling::HiZBuffer hiZBuffer;
        Culling::HiZPass hiZPass;
        VkDescriptorPool cullingDescriptorPool = VK_NULL_HANDLE;
        VkDescriptorSetLayout hiZCopyDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout hiZReduceDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout cullingDescriptorSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout hiZCopyPipelineLayout = VK_NULL_HANDLE;
        VkPipelineLayout hiZReducePipelineLayout = VK_NULL_HANDLE;
        VkPipelineLayout cullingPipelineLayout = VK_NULL_HANDLE;
        VkPipeline hiZCopyPipeline = VK_NULL_HANDLE;
        VkPipeline hiZReducePipeline = VK_NULL_HANDLE;
        VkPipeline cullingPipeline = VK_NULL_HANDLE;
        std::vector<Culling::GPUObjectData> gpuObjects;
        bool hiZValid = false;
        bool cameraMouseLookActive = false;
        bool editorSceneCameraInput = false;
        Entity editorSelectedEntity = NullEntity;
        std::uint32_t editorSelectedRenderable = std::numeric_limits<std::uint32_t>::max();
        Vec3 editorSceneCameraPosition{8.0f, 6.0f, 8.0f};
        float editorSceneCameraYaw = -135.0f;
        // Aim at the ground-level scene center instead of looking too far above
        // it; otherwise the finite ground plane reads like a rotated V-shaped wall.
        float editorSceneCameraPitch = -28.0f;
        bool hasShadowCasters = false;
        struct RenderableRecord {
            Entity entity{NullEntity};
            AABB localBounds{};
            std::size_t batchIndex{0};
            Transform cachedTransform{};
            bool hasCachedTransform{false};
            // A change must reach every frame-in-flight buffer before its bit
            // can be discarded; otherwise the next frame could render stale data.
            uint8_t transformDirtyFrames{0};
            uint8_t materialDirtyFrames{0};
            uint8_t cullingDirtyFrames{0};
        };
        struct InstanceBatch {
            const Mesh* mesh{nullptr};
            uint32_t firstIndex{0};
            uint32_t indexCount{0};
            uint32_t firstInstance{0};
            uint32_t instanceCount{0};
            bool castShadow{true};
            AABB worldBounds{};
        };
        std::vector<RenderableRecord> renderables;
        std::vector<InstanceBatch> instanceBatches;
        std::vector<glm::mat4> instanceModels;
        std::vector<glm::mat4> shadowInstanceModels;
        std::vector<GPUMaterialData> materials;
        std::uint32_t materialSlots{1};
        std::uint64_t lastRenderableRevision = std::numeric_limits<std::uint64_t>::max();
        std::array<std::vector<std::size_t>, MAX_FRAMES_IN_FLIGHT> dirtyTransforms;
        std::array<std::vector<std::size_t>, MAX_FRAMES_IN_FLIGHT> dirtyMaterials;
        std::array<std::vector<std::size_t>, MAX_FRAMES_IN_FLIGHT> dirtyCullingObjects;
        Vec3 sceneCenter{};
        float sceneRadius{1.0f};

        VkCommandPool commandPool{};
        std::vector<VkCommandBuffer> commandBuffers;

        std::vector<VkSemaphore> imageAvailableSemaphores;
        std::vector<VkSemaphore> renderFinishedSemaphores;
        std::vector<VkFence> inFlightFences;
        uint32_t currentFrame = 0;

        bool framebufferResized = false;
        bool cleanedUp = false;

        uint32_t fpsFrameCount = 0;
        double fpsElapsedTime = 0.0;

        static constexpr uint8_t allFrameBits =
            static_cast<uint8_t>((1u << MAX_FRAMES_IN_FLIGHT) - 1u);

        [[nodiscard]] static uint8_t frameBit(const uint32_t frame) noexcept {
            return static_cast<uint8_t>(1u << frame);
        }

        [[nodiscard]] static bool sameTransform(const Transform& lhs,
                                                const Transform& rhs) noexcept {
            const auto sameVector = [](const Vec3& a, const Vec3& b) {
                return a.x() == b.x() && a.y() == b.y() && a.z() == b.z();
            };
            return sameVector(lhs.position, rhs.position) &&
                   sameVector(lhs.rotation, rhs.rotation) &&
                   sameVector(lhs.scale, rhs.scale);
        }

        void markDirty(const std::size_t index,
                       uint8_t RenderableRecord::* dirtyFrames,
                       std::array<std::vector<std::size_t>, MAX_FRAMES_IN_FLIGHT>& dirtyIndices) {
            RenderableRecord& record = renderables[index];
            for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
                const uint8_t bit = frameBit(frame);
                if ((record.*dirtyFrames & bit) == 0) {
                    dirtyIndices[frame].push_back(index);
                }
            }
            record.*dirtyFrames |= allFrameBits;
        }


        void initWindow() {
            if (!window) {
                throw std::invalid_argument("Renderer requires an application-owned SDL window");
            }
        }

        void initVulkan() {
            const char* basePath = SDL_GetBasePath();
            assetManager.set_asset_root(basePath ? std::filesystem::path(basePath) : std::filesystem::path{});
            Assets::register_default_asset_loaders(assetManager);
            assetManager.set_error_handler([](const std::string& message) { std::cerr << "[Assets] " << message << '\n'; });
            createInstance();
            setupDebugMessenger();
            createSurface();
            vulkanDevice.create(instance, surface);
            device = vulkanDevice.logical();
            depthBuffer.initialize(vulkanDevice.physical(), device);
            const VkSampleCountFlagBits requestedSamples =
                antialiasingLevel == AntialiasingLevel::MSAA4x ? VK_SAMPLE_COUNT_4_BIT :
                antialiasingLevel == AntialiasingLevel::MSAA2x ? VK_SAMPLE_COUNT_2_BIT :
                VK_SAMPLE_COUNT_1_BIT;
            msaa.initialize(vulkanDevice.physical(), device, requestedSamples);
            waitForDrawableExtent();
            createSwapChain();
            hdrBuffer.create(vulkanDevice.physical(), device, swapchain.extent());
            msaa.create(swapchain.extent(), HdrBuffer::Format);
            createDepthResources();
            createCommandPool();
            createMaterialTextures();
            createMeshBuffers();
            createInstanceBuffer();
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
            createTonemapPass();
            createUIResources();
            createCommandBuffers();
            createSyncObjects();
            createEditorUiResources();
            // Shader modules no longer need their source text after pipeline
            // creation. Release cache-only asset records before the main loop.
            assetManager.unload_unused();
        }

        // ---------- INSTANCE / DEBUG ----------

        static std::vector<const char*> getRequiredExtensions() {
            Uint32 sdlExtensionCount = 0;
            const char * const *sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);
            if (!sdlExtensions) {
                throw std::runtime_error(std::string("SDL_Vulkan_GetInstanceExtensions error: ") + SDL_GetError());
            }
            std::vector<const char*> extensions(sdlExtensions, sdlExtensions + sdlExtensionCount);

            if (enableValidationLayers) {
                extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            }
            return extensions;
        }

        static bool checkValidationLayerSupport() {
            uint32_t layerCount;
            vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
            std::vector<VkLayerProperties> availableLayers(layerCount);
            vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

            for (const char* layerName : validationLayers) {
                bool layerFound = false;
                for (const auto& layerProperties : availableLayers) {
                    if (strcmp(layerName, layerProperties.layerName) == 0) {
                        layerFound = true;
                        break;
                    }
                }
                if (!layerFound) return false;
            }
            return true;
        }

        static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
            VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
            VkDebugUtilsMessageTypeFlagsEXT messageType,
            const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
            void* pUserData) {
            if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
                std::cerr << "[Vulkan] " << pCallbackData->pMessage << std::endl;
            }
            return VK_FALSE;
        }

        static void populateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& createInfo) {
            createInfo = {};
            createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            createInfo.pfnUserCallback = debugCallback;
        }

        void createInstance() {
            const bool useValidation = enableValidationLayers && checkValidationLayerSupport();

            if (enableValidationLayers && !useValidation) {
                std::cerr << "Validation layers are incorrect\n";
            }

            VkApplicationInfo appInfo{};
            appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
            appInfo.pApplicationName = "Vulkan SDL Cube";
            appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
            appInfo.pEngineName = "No Engine";
            appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
            appInfo.apiVersion = VK_API_VERSION_1_3;

            VkInstanceCreateInfo createInfo{};
            createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
            createInfo.pApplicationInfo = &appInfo;

            const auto extensions = getRequiredExtensions();
            createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
            createInfo.ppEnabledExtensionNames = extensions.data();

            VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
            if (useValidation) {
                createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
                createInfo.ppEnabledLayerNames = validationLayers.data();
                populateDebugMessengerCreateInfo(debugCreateInfo);
                createInfo.pNext = &debugCreateInfo;
            } else {
                createInfo.enabledLayerCount = 0;
                createInfo.pNext = nullptr;
            }

            if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
                throw std::runtime_error("Could not create VkInstance");
            }
        }

        static VkResult CreateDebugUtilsMessengerEXT(VkInstance instance,
            const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
            const VkAllocationCallbacks* pAllocator,
            VkDebugUtilsMessengerEXT* pDebugMessenger) {
            auto func = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
            if (func != nullptr) return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }

        static void DestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const VkAllocationCallbacks* pAllocator) {
            if (const auto func = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT")); func != nullptr) func(instance, debugMessenger, pAllocator);
        }

        void setupDebugMessenger() {
            if (!enableValidationLayers || !checkValidationLayerSupport()) return;
            VkDebugUtilsMessengerCreateInfoEXT createInfo;
            populateDebugMessengerCreateInfo(createInfo);
            if (CreateDebugUtilsMessengerEXT(instance, &createInfo, nullptr, &debugMessenger) != VK_SUCCESS) {
                std::cerr << "Could not create debug messenger.\n";
            }
        }

        void createSurface() {
            if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) {
                throw std::runtime_error(std::string("Could not create VkSurfaceKHR: ") + SDL_GetError());
            }
        }


        void createSwapChain() {
            swapchain.create(window, surface, vulkanDevice);
        }

        void waitForDrawableExtent() const {
            int width = 0;
            int height = 0;
            SDL_GetWindowSizeInPixels(window, &width, &height);
            while (width <= 0 || height <= 0) {
                SDL_Event event;
                SDL_WaitEvent(&event);
                SDL_GetWindowSizeInPixels(window, &width, &height);
            }
        }


        void createDepthResources() {
            depthBuffer.create(swapchain.extent(), msaa.sampleCount());
            hiZDepthBuffer.initialize(vulkanDevice.physical(), device);
            if (msaa.enabled()) {
                hiZDepthBuffer.create(swapchain.extent(), VK_SAMPLE_COUNT_1_BIT,
                                      depthBuffer.format());
            }
        }

        void destroyDepthResources() {
            hiZDepthBuffer.destroy();
            depthBuffer.destroy();
        }

        void createShadowPass() {
            std::vector<VkBuffer> buffers;
            std::vector<VkBuffer> gpuMaterialBuffers;
            buffers.reserve(uniformBuffers.size());
            gpuMaterialBuffers.reserve(materialBuffers.size());
            for (const Buffer& buffer : uniformBuffers) {
                buffers.push_back(buffer.handle());
            }
            for (const Buffer& buffer : materialBuffers) {
                gpuMaterialBuffers.push_back(buffer.handle());
            }
            shadowPass.create(vulkanDevice.physical(), device, buffers,
                              gpuMaterialBuffers, materialTextureDescriptors,
                              sizeof(UniformBufferObject), assetManager);
        }

        void createSceneDescriptorPass() {
            std::vector<VkBuffer> buffers;
            std::vector<VkBuffer> gpuMaterialBuffers;
            buffers.reserve(sceneUniformBuffers.size());
            gpuMaterialBuffers.reserve(materialBuffers.size());
            for (const Buffer& buffer : sceneUniformBuffers) buffers.push_back(buffer.handle());
            for (const Buffer& buffer : materialBuffers) gpuMaterialBuffers.push_back(buffer.handle());
            sceneDescriptorPass.create(vulkanDevice.physical(), device, buffers,
                                       gpuMaterialBuffers, materialTextureDescriptors,
                                       sizeof(UniformBufferObject), assetManager);
        }

        void createForwardPass() {
            forwardPass.create(device, HdrBuffer::Format, depthBuffer.format(),
                               msaa.sampleCount(),
                               shadowPass.descriptorSetLayout(), assetManager);
            if (msaa.enabled()) {
                hiZDepthPrepass.create(device, HdrBuffer::Format, hiZDepthBuffer.format(),
                                       VK_SAMPLE_COUNT_1_BIT,
                                       shadowPass.descriptorSetLayout(), assetManager);
            }
        }

        void createParticleResources() {
            if (!scene.isParticleScene()) {
                return;
            }

            if (!particleSystem) {
                particleSystem = std::make_unique<Particles::ParticleSystem>(
                    device, vulkanDevice.physical(), vulkanDevice.graphicsQueue(), commandPool, 8192);
                particleSystem->setEmitter(scene.particleEmitter);
            }

            GraphicsPipelineOptions options{};
            options.colorFormat = HdrBuffer::Format;
            options.depthFormat = depthBuffer.format();
            options.samples = msaa.sampleCount();
            options.existingRenderPass = forwardPass.renderPass();
            options.vertexShader = "shaders/particle.vert.spv";
            options.fragmentShader = "shaders/particle.frag.spv";
            options.assetManager = &assetManager;
            options.cullMode = VK_CULL_MODE_NONE;
            options.depthWriteEnable = VK_FALSE;
            options.depthTestEnable = VK_TRUE;
            options.alphaBlendEnable = VK_TRUE;
            options.descriptorSetLayouts = {particleSystem->descriptorSetLayout()};
            options.vertexBindings = {{0, sizeof(float) * 4, VK_VERTEX_INPUT_RATE_VERTEX}};
            options.vertexAttributes = {
                {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
                {1, 0, VK_FORMAT_R32G32_SFLOAT, sizeof(float) * 2},
            };
            particlePipeline.create(device, options);

            if (particleComputePipeline == VK_NULL_HANDLE) {
                VkPushConstantRange pushConstants{};
                pushConstants.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                pushConstants.offset = 0;
                pushConstants.size = sizeof(Particles::ParticleSimulationData);
                VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
                const VkDescriptorSetLayout descriptorSetLayout = particleSystem->descriptorSetLayout();
                layout.setLayoutCount = 1;
                layout.pSetLayouts = &descriptorSetLayout;
                layout.pushConstantRangeCount = 1;
                layout.pPushConstantRanges = &pushConstants;
                if (vkCreatePipelineLayout(device, &layout, nullptr, &particleComputePipelineLayout) != VK_SUCCESS) {
                    throw std::runtime_error("Could not create particle compute pipeline layout");
                }
                particleComputePipeline = createComputePipeline(
                    "shaders/particle_update.comp.spv", particleComputePipelineLayout);
            }
        }

        void reconfigureAntialiasing() {
            if (device == VK_NULL_HANDLE) return;

            // Nothing may reference the old render passes or attachments while
            // they are being replaced. This also guarantees that the old
            // command buffers have finished before their pipelines disappear.
            vkDeviceWaitIdle(device);

            destroyCullingResources();
            destroyEditorUiResources();
            canvasRenderer.destroy();
            tonemapPass.destroy();

            if (hiZDepthPrepassFramebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device, hiZDepthPrepassFramebuffer, nullptr);
                hiZDepthPrepassFramebuffer = VK_NULL_HANDLE;
            }
            if (hdrFramebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device, hdrFramebuffer, nullptr);
                hdrFramebuffer = VK_NULL_HANDLE;
            }
            destroySceneViewportResources();

            particlePipeline.destroy();
            skyPass.destroy();
            sceneSkyPass.destroy();
            forwardPass.destroy();
            hiZDepthPrepass.destroy();

            msaa.destroy();
            hdrBuffer.destroy();
            destroyDepthResources();

            const VkSampleCountFlagBits requestedSamples =
                antialiasingLevel == AntialiasingLevel::MSAA4x ? VK_SAMPLE_COUNT_4_BIT :
                antialiasingLevel == AntialiasingLevel::MSAA2x ? VK_SAMPLE_COUNT_2_BIT :
                VK_SAMPLE_COUNT_1_BIT;
            msaa.initialize(vulkanDevice.physical(), device, requestedSamples);
            hdrBuffer.create(vulkanDevice.physical(), device, swapchain.extent());
            msaa.create(swapchain.extent(), HdrBuffer::Format);
            createDepthResources();

            createForwardPass();
            createParticleResources();
            createSkyPass();
            createSceneSkyPass();
            createFramebuffers();
            createSceneViewportResources();
            createTonemapPass();
            createUIResources();
            createEditorUiResources();
            createCullingResources();
        }

        void createSkyPass() {
            std::vector<VkBuffer> buffers;
            buffers.reserve(uniformBuffers.size());
            for (const Buffer& buffer : uniformBuffers) {
                buffers.push_back(buffer.handle());
            }
            skyPass.create(vulkanDevice.physical(), device, commandPool,
                           vulkanDevice.graphicsQueue(), forwardPass.renderPass(),
                           HdrBuffer::Format, msaa.sampleCount(), buffers,
                           sizeof(UniformBufferObject), assetManager,
                           vulkanDevice.allocator());
        }

        void createSceneSkyPass() {
            std::vector<VkBuffer> buffers;
            buffers.reserve(sceneUniformBuffers.size());
            for (const Buffer& buffer : sceneUniformBuffers) buffers.push_back(buffer.handle());
            sceneSkyPass.create(vulkanDevice.physical(), device, commandPool,
                                vulkanDevice.graphicsQueue(), forwardPass.renderPass(),
                                HdrBuffer::Format, msaa.sampleCount(), buffers,
                                sizeof(UniformBufferObject), assetManager,
                                vulkanDevice.allocator());
        }

        void createTonemapPass() {
            tonemapPass.create(device, swapchain.format(), swapchain.extent(),
                               swapchain.imageViews(), hdrBuffer.imageView(),
                               hdrBuffer.sampler(), assetManager);
        }

        void createUIResources() {
            const VkExtent2D extent = swapchain.extent();
            scene.uiCanvas().resize(extent.width, extent.height);

            if (!fpsFontTexture.valid()) {
                const auto& atlas = scene.uiFontAtlas();
                fpsFontTexture.create(vulkanDevice.physical(), device, commandPool,
                                      vulkanDevice.graphicsQueue(), atlas.width(),
                                      atlas.height(), atlas.pixels(), TextureColorSpace::Linear,
                                      false, vulkanDevice.allocator(), TexturePixelFormat::R8);
            }

            canvasRenderer.create(
                vulkanDevice.physical(), device, swapchain.format(), extent,
                swapchain.imageViews(), MAX_FRAMES_IN_FLIGHT, assetManager,
                fpsFontTexture.imageView(), fpsFontTexture.sampler(),
                vulkanDevice.allocator());
        }

        void createEditorUiResources() {
            if (ImGui::GetCurrentContext() == nullptr) return;
            VkAttachmentDescription color{};
            color.format = swapchain.format(); color.samples = VK_SAMPLE_COUNT_1_BIT;
            // In editor mode the swapchain is UI background, not a second
            // full-screen Game View. The selected viewport is sampled only by
            // ImGui::Image, so clear the presentation image before drawing UI.
            color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            // The first acquired swapchain image is still UNDEFINED. The
            // pass clears it, so preserving PRESENT_SRC_KHR is unnecessary
            // and makes the first submit use an invalid old layout.
            color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &reference;
            VkSubpassDependency dependency{};
            dependency.srcSubpass = VK_SUBPASS_EXTERNAL; dependency.dstSubpass = 0;
            dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
            passInfo.attachmentCount = 1; passInfo.pAttachments = &color;
            passInfo.subpassCount = 1; passInfo.pSubpasses = &subpass;
            passInfo.dependencyCount = 1; passInfo.pDependencies = &dependency;
            if (vkCreateRenderPass(device, &passInfo, nullptr, &editorUiRenderPass) != VK_SUCCESS) {
                throw std::runtime_error("Could not create ImGui render pass");
            }
            editorUiFramebuffers.resize(swapchain.imageCount());
            for (std::size_t index = 0; index < editorUiFramebuffers.size(); ++index) {
                const VkImageView view = swapchain.imageViews()[index];
                VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
                framebuffer.renderPass = editorUiRenderPass; framebuffer.attachmentCount = 1;
                framebuffer.pAttachments = &view; framebuffer.width = swapchain.extent().width;
                framebuffer.height = swapchain.extent().height; framebuffer.layers = 1;
                if (vkCreateFramebuffer(device, &framebuffer, nullptr, &editorUiFramebuffers[index]) != VK_SUCCESS) {
                    throw std::runtime_error("Could not create ImGui framebuffer");
                }
            }
            // ImGui_ImplVulkan_Shutdown() destroys platform windows as part of
            // its viewport cleanup. The SDL backend therefore has to be
            // initialized again after every Vulkan-backend rebuild; otherwise
            // its stale state rejects all events for the main SDL window.
            if (!ImGui_ImplSDL3_InitForVulkan(window)) {
                throw std::runtime_error("Could not initialize ImGui SDL backend");
            }
            ImGui_ImplVulkan_InitInfo info{};
            info.ApiVersion = VK_API_VERSION_1_3; info.Instance = instance;
            info.PhysicalDevice = vulkanDevice.physical(); info.Device = device;
            info.QueueFamily = vulkanDevice.graphicsQueueFamily(); info.Queue = vulkanDevice.graphicsQueue();
            info.DescriptorPoolSize = 128; info.MinImageCount = 2;
            info.ImageCount = static_cast<uint32_t>(swapchain.imageCount());
            info.PipelineInfoMain.RenderPass = editorUiRenderPass;
            info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
            if (!ImGui_ImplVulkan_Init(&info)) throw std::runtime_error("Could not initialize ImGui Vulkan backend");
            gameViewportDescriptor = ImGui_ImplVulkan_AddTexture(hdrBuffer.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            sceneViewportDescriptor = ImGui_ImplVulkan_AddTexture(sceneViewportTarget.color().imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            editorUiActive = true;
        }

        void destroyEditorUiResources() noexcept {
            if (editorUiActive) {
                if (gameViewportDescriptor != VK_NULL_HANDLE) ImGui_ImplVulkan_RemoveTexture(gameViewportDescriptor);
                if (sceneViewportDescriptor != VK_NULL_HANDLE) ImGui_ImplVulkan_RemoveTexture(sceneViewportDescriptor);
            }
            // Do not rely only on editorUiActive here. If initialization failed
            // halfway through, ImGui can still own a renderer backend and the
            // next Vulkan rebuild would assert in ImGui_ImplVulkan_Init.
            if (ImGui::GetCurrentContext() != nullptr &&
                ImGui::GetIO().BackendRendererUserData != nullptr) {
                ImGui_ImplVulkan_Shutdown();
            }
            // Vulkan shutdown clears the main viewport's PlatformHandle.
            // Shut down SDL too so createEditorUiResources() can register the
            // application window again on the next renderer rebuild.
            if (ImGui::GetCurrentContext() != nullptr &&
                ImGui::GetIO().BackendPlatformUserData != nullptr) {
                ImGui_ImplSDL3_Shutdown();
            }
            gameViewportDescriptor = sceneViewportDescriptor = VK_NULL_HANDLE;
            editorUiActive = false;
            for (VkFramebuffer framebuffer : editorUiFramebuffers) if (framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device, framebuffer, nullptr);
            editorUiFramebuffers.clear();
            if (editorUiRenderPass != VK_NULL_HANDLE) vkDestroyRenderPass(device, editorUiRenderPass, nullptr);
            editorUiRenderPass = VK_NULL_HANDLE;
        }

        void createMaterialTextures() {
            constexpr std::array<std::uint8_t, 4> white = {255, 255, 255, 255};
            fallbackMaterialTexture.create(
                vulkanDevice.physical(), device, commandPool, vulkanDevice.graphicsQueue(),
                1, 1, white, TextureColorSpace::SRGB, false, vulkanDevice.allocator());
            const VkDescriptorImageInfo fallback{
                fallbackMaterialTexture.sampler(), fallbackMaterialTexture.imageView(),
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            materialTextureDescriptors.assign(MaxMaterialTextures, fallback);

            std::unordered_set<const Mesh*> uploaded;
            registry.view<MeshRenderer>([&](const Entity, const MeshRenderer& renderer) {
                if (!renderer.hasMesh() || !uploaded.insert(renderer.mesh.get()).second) return;
                const Mesh& mesh = *renderer.mesh;
                const std::uint32_t offset = static_cast<std::uint32_t>(materialTextures.size() + 1);
                if (mesh.images.size() > MaxMaterialTextures - offset) {
                    throw std::runtime_error("GLB scene exceeds the material texture limit");
                }
                meshTextureOffsets.emplace(&mesh, offset);
                for (std::size_t i = 0; i < mesh.images.size(); ++i) {
                    const Mesh::Image& image = mesh.images[i];
                    const bool isBaseColorTexture = std::ranges::any_of(
                        mesh.materials, [i](const PBRMaterial& material) {
                            return material.baseColorTexture == static_cast<std::int32_t>(i);
                        });
                    if (image.width == 0 || image.height == 0 || image.rgbaPixels.empty()) {
                        materialTextures.emplace_back();
                        continue;
                    }
                    Texture2D texture;
                    texture.create(vulkanDevice.physical(), device, commandPool,
                                   vulkanDevice.graphicsQueue(), image.width, image.height,
                                   image.rgbaPixels, isBaseColorTexture ? TextureColorSpace::SRGB
                                                                        : TextureColorSpace::Linear,
                                   true,
                                   vulkanDevice.allocator());
                    materialTextureDescriptors[offset + i] = {
                        texture.sampler(), texture.imageView(),
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                    materialTextures.push_back(std::move(texture));
                }
            });
        }

        void createMeshBuffers() {
            Mesh sceneMesh;
            renderables.reserve(registry.size());
            renderables.clear();
            instanceBatches.clear();
            instanceBatches.reserve(registry.size());
            glm::vec3 sceneMinimum{std::numeric_limits<float>::max()};
            glm::vec3 sceneMaximum{std::numeric_limits<float>::lowest()};
            // Each MeshRenderer retains its own draw range, but identical
            // meshes contribute their geometry to the GPU buffers only once.
            struct MeshUploadRecord {
                uint32_t firstIndex;
                AABB localBounds;
            };
            struct BatchKey {
                const Mesh* mesh;
                bool castShadow;
                uint32_t cullingBatch;

                bool operator==(const BatchKey& other) const noexcept {
                    return mesh == other.mesh && castShadow == other.castShadow &&
                           cullingBatch == other.cullingBatch;
                }
            };
            struct BatchKeyHash {
                std::size_t operator()(const BatchKey& key) const noexcept {
                    const auto meshHash = std::hash<const Mesh*>{}(key.mesh);
                    const auto batchHash = std::hash<uint32_t>{}(key.cullingBatch);
                    return meshHash ^ (batchHash + static_cast<std::size_t>(key.castShadow) +
                                       0x9e3779b9u + (meshHash << 6u) + (meshHash >> 2u));
                }
            };
            std::unordered_map<const Mesh*, MeshUploadRecord> uploadedMeshes;
            uploadedMeshes.reserve(registry.size());
            std::unordered_map<BatchKey, std::size_t, BatchKeyHash> batchIndices;
            batchIndices.reserve(registry.size());
            std::unordered_set<const Mesh*> uniqueMeshes;
            uniqueMeshes.reserve(registry.size());
            std::size_t vertexCapacity = 0;
            std::size_t indexCapacity = 0;
            materialSlots = 1;
            registry.view<MeshRenderer>([&](const Entity, const MeshRenderer& renderer) {
                if (!renderer.hasMesh() || !uniqueMeshes.insert(renderer.mesh.get()).second) {
                    return;
                }
                vertexCapacity += renderer.mesh->vertices.size();
                indexCapacity += renderer.mesh->indices.size();
                materialSlots = std::max(materialSlots, static_cast<std::uint32_t>(
                    std::max<std::size_t>(1, renderer.mesh->materials.size())));
            });
            sceneMesh.vertices.reserve(vertexCapacity);
            sceneMesh.indices.reserve(indexCapacity);
            registry.view<Transform, MeshRenderer>(
                [&](const Entity entity, const Transform& transform, MeshRenderer& renderer) {
                    if (!renderer.hasMesh()) {
                        return;
                    }

                    const Mesh* const mesh = renderer.mesh.get();
                    AABB localBounds;
                    if (optimizationFeatures.meshDeduplication) {
                        const auto existing = uploadedMeshes.find(mesh);
                        if (existing != uploadedMeshes.end()) {
                        renderer.firstIndex = existing->second.firstIndex;
                        localBounds = existing->second.localBounds;
                        } else {
                            if (sceneMesh.vertices.size() + mesh->vertices.size() >
                                    std::numeric_limits<uint32_t>::max() ||
                                sceneMesh.indices.size() + mesh->indices.size() >
                                    std::numeric_limits<uint32_t>::max()) {
                                throw std::runtime_error("Scene geometry exceeds 32-bit draw limits");
                            }
                            const uint32_t vertexOffset = sceneMesh.vertexCount();
                            renderer.firstIndex = sceneMesh.indexCount();
                            sceneMesh.vertices.insert(sceneMesh.vertices.end(),
                                                      mesh->vertices.begin(), mesh->vertices.end());
                            for (const uint32_t index : mesh->indices) {
                                sceneMesh.indices.push_back(vertexOffset + index);
                            }
                            localBounds = {
                            .min = Vec3{std::numeric_limits<float>::max(),
                                        std::numeric_limits<float>::max(),
                                        std::numeric_limits<float>::max()},
                            .max = Vec3{std::numeric_limits<float>::lowest(),
                                        std::numeric_limits<float>::lowest(),
                                        std::numeric_limits<float>::lowest()},
                            };
                            for (const Vertex& vertex : mesh->vertices) {
                                localBounds.min.setX(std::min(localBounds.min.x(), vertex.position.x()));
                                localBounds.min.setY(std::min(localBounds.min.y(), vertex.position.y()));
                                localBounds.min.setZ(std::min(localBounds.min.z(), vertex.position.z()));
                                localBounds.max.setX(std::max(localBounds.max.x(), vertex.position.x()));
                                localBounds.max.setY(std::max(localBounds.max.y(), vertex.position.y()));
                                localBounds.max.setZ(std::max(localBounds.max.z(), vertex.position.z()));
                            }
                            uploadedMeshes.emplace(mesh, MeshUploadRecord{renderer.firstIndex, localBounds});
                        }
                    } else {
                        if (sceneMesh.vertices.size() + mesh->vertices.size() >
                                std::numeric_limits<uint32_t>::max() ||
                            sceneMesh.indices.size() + mesh->indices.size() >
                                std::numeric_limits<uint32_t>::max()) {
                            throw std::runtime_error("Scene geometry exceeds 32-bit draw limits");
                        }
                        const uint32_t vertexOffset = sceneMesh.vertexCount();
                        renderer.firstIndex = sceneMesh.indexCount();
                        sceneMesh.vertices.insert(sceneMesh.vertices.end(),
                                                  mesh->vertices.begin(), mesh->vertices.end());
                        for (const uint32_t index : mesh->indices) {
                            sceneMesh.indices.push_back(vertexOffset + index);
                        }
                        localBounds = {
                        .min = Vec3{std::numeric_limits<float>::max(),
                                    std::numeric_limits<float>::max(),
                                    std::numeric_limits<float>::max()},
                        .max = Vec3{std::numeric_limits<float>::lowest(),
                                    std::numeric_limits<float>::lowest(),
                                    std::numeric_limits<float>::lowest()},
                        };
                        for (const Vertex& vertex : mesh->vertices) {
                        localBounds.min.setX(std::min(localBounds.min.x(), vertex.position.x()));
                        localBounds.min.setY(std::min(localBounds.min.y(), vertex.position.y()));
                        localBounds.min.setZ(std::min(localBounds.min.z(), vertex.position.z()));
                        localBounds.max.setX(std::max(localBounds.max.x(), vertex.position.x()));
                        localBounds.max.setY(std::max(localBounds.max.y(), vertex.position.y()));
                        localBounds.max.setZ(std::max(localBounds.max.z(), vertex.position.z()));
                        }
                    }
                    const AABB worldBounds =
                        localBounds.transformed(transform.matrix().native());
                    const bool castShadow = renderer.castShadow;
                    const BatchKey batchKey{mesh, castShadow, renderer.cullingBatch};
                    const auto [batchIt, inserted] = optimizationFeatures.instancedRendering
                        ? batchIndices.try_emplace(batchKey, instanceBatches.size())
                        : std::pair{batchIndices.end(), true};
                    const std::size_t batchIndex = optimizationFeatures.instancedRendering
                        ? batchIt->second : instanceBatches.size();
                    if (inserted) {
                        instanceBatches.push_back(InstanceBatch{
                            .mesh = mesh,
                            .firstIndex = renderer.firstIndex,
                            .indexCount = mesh->indexCount(),
                            .firstInstance = static_cast<uint32_t>(renderables.size()),
                            .instanceCount = 0,
                            .castShadow = castShadow,
                            .worldBounds = worldBounds,
                        });
                    }
                    InstanceBatch& batch = instanceBatches[batchIndex];
                    if (batch.instanceCount == 0) {
                        batch.worldBounds = worldBounds;
                    } else {
                        batch.worldBounds.min = Vec3{
                            std::min(batch.worldBounds.min.x(), worldBounds.min.x()),
                            std::min(batch.worldBounds.min.y(), worldBounds.min.y()),
                            std::min(batch.worldBounds.min.z(), worldBounds.min.z())};
                        batch.worldBounds.max = Vec3{
                            std::max(batch.worldBounds.max.x(), worldBounds.max.x()),
                            std::max(batch.worldBounds.max.y(), worldBounds.max.y()),
                            std::max(batch.worldBounds.max.z(), worldBounds.max.z())};
                    }
                    ++batch.instanceCount;
                    renderables.push_back({entity, localBounds, batchIndex});
                    sceneMinimum = glm::min(sceneMinimum, worldBounds.min.native());
                    sceneMaximum = glm::max(sceneMaximum, worldBounds.max.native());
                });

            if (sceneMesh.empty()) {
                throw std::runtime_error("Scene contains no renderable geometry");
            }

            hasShadowCasters = false;
            for (const InstanceBatch& batch : instanceBatches) {
                if (batch.castShadow) {
                    hasShadowCasters = true;
                    break;
                }
            }

            const glm::vec3 center = (sceneMinimum + sceneMaximum) * 0.5f;
            const glm::vec3 halfExtent = (sceneMaximum - sceneMinimum) * 0.5f;
            sceneCenter = Vec3{center};
            sceneRadius = std::max({halfExtent.x, halfExtent.y, halfExtent.z, 1.0f});

            vertexBuffer.createDeviceLocal(vulkanDevice.physical(), device, sceneMesh.vertices.data(),
                sizeof(Vertex) * sceneMesh.vertices.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
            indexBuffer.createDeviceLocal(vulkanDevice.physical(), device, sceneMesh.indices.data(),
                sizeof(uint32_t) * sceneMesh.indices.size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        }

        void createInstanceBuffer() {
            instanceModels.resize(renderables.size());
            shadowInstanceModels.resize(renderables.size());
            materials.resize(renderables.size() * materialSlots);
            updateRenderableBuffers();
            for (Buffer& buffer : instanceBuffers) {
                buffer.createHostVisible(vulkanDevice.physical(), device,
                    sizeof(glm::mat4) * instanceModels.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                    vulkanDevice.allocator());
                buffer.update(instanceModels.data(), sizeof(glm::mat4) * instanceModels.size());
            }
            for (Buffer& buffer : shadowInstanceBuffers) {
                buffer.createHostVisible(vulkanDevice.physical(), device,
                    sizeof(glm::mat4) * shadowInstanceModels.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                    vulkanDevice.allocator());
                buffer.update(shadowInstanceModels.data(), sizeof(glm::mat4) * shadowInstanceModels.size());
            }
            for (Buffer& buffer : materialBuffers) {
                buffer.createHostVisible(vulkanDevice.physical(), device,
                    sizeof(GPUMaterialData) * materials.size(),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vulkanDevice.allocator());
                buffer.update(materials.data(), sizeof(GPUMaterialData) * materials.size());
            }

            // Every instance/material buffer has just received the complete
            // CPU snapshot, so no incremental upload is pending for it.
            for (RenderableRecord& record : renderables) {
                record.transformDirtyFrames = 0;
                record.materialDirtyFrames = 0;
            }
            for (auto& indices : dirtyTransforms) indices.clear();
            for (auto& indices : dirtyMaterials) indices.clear();
        }

        template <typename T>
        void uploadDirtyRanges(const Buffer& buffer, const std::vector<T>& data,
                               uint8_t RenderableRecord::* dirtyFrames,
                               const uint8_t bit) const {
            std::size_t rangeBegin = 0;
            while (rangeBegin < renderables.size()) {
                while (rangeBegin < renderables.size() &&
                       (renderables[rangeBegin].*dirtyFrames & bit) == 0) {
                    ++rangeBegin;
                }
                std::size_t rangeEnd = rangeBegin;
                while (rangeEnd < renderables.size() &&
                       (renderables[rangeEnd].*dirtyFrames & bit) != 0) {
                    ++rangeEnd;
                }
                if (rangeBegin != rangeEnd) {
                    buffer.update(data.data() + rangeBegin,
                                  sizeof(T) * (rangeEnd - rangeBegin),
                                  sizeof(T) * rangeBegin);
                }
                rangeBegin = rangeEnd;
            }
        }

        template <typename T>
        void uploadDirtyIndices(const Buffer& buffer, const std::vector<T>& data,
                                uint8_t RenderableRecord::* dirtyFrames,
                                const std::vector<std::size_t>& indices) const {
            std::size_t rangeStart = 0;
            while (rangeStart < indices.size()) {
                std::size_t rangeEnd = rangeStart + 1;
                while (rangeEnd < indices.size() &&
                       indices[rangeEnd] == indices[rangeEnd - 1] + 1) {
                    ++rangeEnd;
                }
                const std::size_t first = indices[rangeStart];
                buffer.update(data.data() + first, sizeof(T) * (rangeEnd - rangeStart),
                              sizeof(T) * first);
                rangeStart = rangeEnd;
            }
        }

        void clearDirtyIndices(uint8_t RenderableRecord::* dirtyFrames,
                               std::vector<std::size_t>& indices, const uint8_t bit) {
            for (const std::size_t index : indices) {
                renderables[index].*dirtyFrames &= static_cast<uint8_t>(~bit);
            }
            indices.clear();
        }

        // Each frame in flight owns a separate GPU buffer.  A scene mutation
        // must therefore be uploaded once per buffer, even after the registry
        // revision itself has stopped changing.
        void uploadPendingRenderableBuffers() {
            if (instanceBuffers[currentFrame].handle() == VK_NULL_HANDLE) return;

            const uint8_t bit = frameBit(currentFrame);
            uploadDirtyIndices(instanceBuffers[currentFrame], instanceModels,
                               &RenderableRecord::transformDirtyFrames,
                               dirtyTransforms[currentFrame]);
            uploadDirtyIndices(shadowInstanceBuffers[currentFrame], shadowInstanceModels,
                               &RenderableRecord::transformDirtyFrames,
                               dirtyTransforms[currentFrame]);
            clearDirtyIndices(&RenderableRecord::transformDirtyFrames,
                              dirtyTransforms[currentFrame], bit);
            for (const std::size_t index : dirtyMaterials[currentFrame]) {
                materialBuffers[currentFrame].update(
                    materials.data() + index * materialSlots,
                    sizeof(GPUMaterialData) * materialSlots,
                    sizeof(GPUMaterialData) * index * materialSlots);
            }
            clearDirtyIndices(&RenderableRecord::materialDirtyFrames,
                              dirtyMaterials[currentFrame], bit);
        }

        void updateRenderableBuffers() {
            const std::uint64_t revision = registry.mutationRevision();
            if (revision == lastRenderableRevision) {
                uploadPendingRenderableBuffers();
                return;
            }

            const Registry& readRegistry = registry;
            for (std::size_t index = 0; index < renderables.size(); ++index) {
                const Entity entity = renderables[index].entity;
                const Transform& transform = readRegistry.get<Transform>(entity);
                const MeshRenderer& renderer = readRegistry.get<MeshRenderer>(entity);
                RenderableRecord& record = renderables[index];
                if (!optimizationFeatures.transformCaching ||
                    !record.hasCachedTransform || !sameTransform(record.cachedTransform, transform)) {
                    const glm::mat4 model = transform.matrix().native();
                    shadowInstanceModels[index] = model;
                    instanceModels[index] = model;
                    record.cachedTransform = transform;
                    record.hasCachedTransform = true;
                    markDirty(index, &RenderableRecord::transformDirtyFrames, dirtyTransforms);
                    markDirty(index, &RenderableRecord::cullingDirtyFrames, dirtyCullingObjects);
                }
                const Mesh& mesh = *renderer.mesh;
                bool materialChanged = false;
                for (std::uint32_t slot = 0; slot < materialSlots; ++slot) {
                    const PBRMaterial source = mesh.materials.empty()
                        ? renderer.material
                        : (slot < mesh.materials.size() ? mesh.materials[slot] : PBRMaterial{});
                    const auto textureIndex = [&](const std::int32_t localIndex) {
                        const auto offset = meshTextureOffsets.find(&mesh);
                        if (localIndex < 0 || offset == meshTextureOffsets.end() ||
                            static_cast<std::size_t>(localIndex) >= mesh.images.size()) return -1;
                        return static_cast<std::int32_t>(offset->second + localIndex);
                    };
                    const GPUMaterialData material{
                        glm::vec4{source.baseColor.r(), source.baseColor.g(),
                                  source.baseColor.b(), source.metallic},
                        glm::vec4{source.roughness, source.ambientOcclusion,
                                  source.alphaCutoff, source.normalScale},
                        glm::ivec4{textureIndex(source.baseColorTexture),
                                   textureIndex(source.metallicRoughnessTexture),
                                   textureIndex(source.normalTexture),
                                   (source.doubleSided ? 1 : 0) | (source.alphaBlend ? 2 : 0)},
                    };
                    GPUMaterialData& destination = materials[index * materialSlots + slot];
                    if (!optimizationFeatures.materialCaching ||
                        std::memcmp(&destination, &material, sizeof(material)) != 0) {
                        destination = material;
                        materialChanged = true;
                    }
                }
                if (materialChanged) {
                    markDirty(index, &RenderableRecord::materialDirtyFrames, dirtyMaterials);
                }
            }
            if (gpuObjects.size() == instanceBatches.size()) {
                std::vector<AABB> batchBounds(instanceBatches.size());
                std::vector<bool> initialized(instanceBatches.size(), false);
                for (std::size_t index = 0; index < renderables.size(); ++index) {
                    const RenderableRecord& record = renderables[index];
                    const Transform& transform = readRegistry.get<Transform>(record.entity);
                    const AABB worldBounds =
                        record.localBounds.transformed(transform.matrix().native());
                    AABB& bounds = batchBounds[record.batchIndex];
                    if (!initialized[record.batchIndex]) {
                        bounds = worldBounds;
                        initialized[record.batchIndex] = true;
                    } else {
                        bounds.min = Vec3{
                            std::min(bounds.min.x(), worldBounds.min.x()),
                            std::min(bounds.min.y(), worldBounds.min.y()),
                            std::min(bounds.min.z(), worldBounds.min.z())};
                        bounds.max = Vec3{
                            std::max(bounds.max.x(), worldBounds.max.x()),
                            std::max(bounds.max.y(), worldBounds.max.y()),
                            std::max(bounds.max.z(), worldBounds.max.z())};
                    }
                }
                for (std::size_t batchIndex = 0; batchIndex < instanceBatches.size(); ++batchIndex) {
                    instanceBatches[batchIndex].worldBounds = batchBounds[batchIndex];
                    auto& object = gpuObjects[batchIndex];
                    object.localAabbMin = {batchBounds[batchIndex].min.x(), batchBounds[batchIndex].min.y(), batchBounds[batchIndex].min.z(), 0.0f};
                    object.localAabbMax = {batchBounds[batchIndex].max.x(), batchBounds[batchIndex].max.y(), batchBounds[batchIndex].max.z(), 0.0f};
                    object.model = {};
                    object.model.data[0] = 1.0f;
                    object.model.data[5] = 1.0f;
                    object.model.data[10] = 1.0f;
                    object.model.data[15] = 1.0f;
                }
                for (Buffer& buffer : cullingObjectBuffers) {
                    if (buffer.handle() != VK_NULL_HANDLE) {
                        buffer.update(gpuObjects.data(), sizeof(Culling::GPUObjectData) * gpuObjects.size());
                    }
                }
            }
            uploadPendingRenderableBuffers();
            lastRenderableRevision = revision;
        }

        [[nodiscard]] bool canUseHiZOcclusionCulling() const noexcept {
            return optimizationFeatures.gpuCulling && optimizationFeatures.occlusionCulling;
        }

        void updateCullingUniformBuffer(const uint32_t frame) const {
            Culling::CullingUniformData data{};
            if (!camera) {
                throw std::runtime_error("Camera must be initialized before culling");
            }
            const glm::mat4 viewProjection = camera->projectionMatrix().native() * camera->viewMatrix().native();
            std::memcpy(data.viewProjection.data, &viewProjection, sizeof(viewProjection));
            data.objectCount = static_cast<uint32_t>(gpuObjects.size());
            data.maxDrawCount = data.objectCount;
            data.hizMipCount = hiZBuffer.mipCount();
            data.enableOcclusionCulling = canUseHiZOcclusionCulling() ? 1u : 0u;
            data.viewportWidth = static_cast<float>(swapchain.extent().width);
            data.viewportHeight = static_cast<float>(swapchain.extent().height);
            data.depthBias = 0.0025f;
            data.aabbExpansion = 0.01f;
            // Never reject objects using an uninitialized hierarchy.
            data.cameraCut = hiZValid ? 0u : 1u;
            data.shadowPass = 0;
            data.enableFrustumCulling = optimizationFeatures.gpuCulling ? 1u : 0u;
            cullingUniformBuffers[frame].update(&data, sizeof(data));
        }

        void updateShadowCullingUniformBuffer(const uint32_t frame) const {
            Culling::CullingUniformData data{};
            const glm::mat4 lightViewProjection = lightSpaceMatrix().native();
            std::memcpy(data.viewProjection.data, &lightViewProjection, sizeof(lightViewProjection));
            data.objectCount = static_cast<uint32_t>(gpuObjects.size());
            data.maxDrawCount = data.objectCount;
            // Shadow culling uses only the light frustum. Camera Hi-Z cannot safely
            // reject casters which are invisible to the camera but visible to the light.
            data.enableOcclusionCulling = 0;
            data.aabbExpansion = 0.01f;
            data.cameraCut = 1;
            data.shadowPass = 1;
            data.enableFrustumCulling = optimizationFeatures.gpuCulling ? 1u : 0u;
            shadowCullingUniformBuffers[frame].update(&data, sizeof(data));
        }

        void createUniformBuffers() {
            for (Buffer& buffer : uniformBuffers) {
                buffer.createHostVisible(vulkanDevice.physical(), device, sizeof(UniformBufferObject),
                                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, vulkanDevice.allocator());
            }
        }

        void createSceneUniformBuffers() {
            for (Buffer& buffer : sceneUniformBuffers) {
                buffer.createHostVisible(vulkanDevice.physical(), device, sizeof(UniformBufferObject),
                                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, vulkanDevice.allocator());
            }
        }

        VkPipeline createComputePipeline(const char* shaderPath, VkPipelineLayout layout) {
            const auto shader = vkutil::loadShaderModule(device, assetManager, shaderPath);
            VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            stage.module = shader.get();
            stage.pName = "main";
            VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            info.stage = stage;
            info.layout = layout;
            VkPipeline pipeline = VK_NULL_HANDLE;
            if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
                throw std::runtime_error("Could not create Hi-Z compute pipeline");
            }
            return pipeline;
        }

        void createCullingResources() {
            hiZValid = false;
            const auto objectCount = static_cast<uint32_t>(instanceBatches.size());
            if (objectCount == 0) return;

            hiZBuffer.create(vulkanDevice.physical(), device, swapchain.extent().width, swapchain.extent().height);

            constexpr VkDescriptorSetLayoutBinding copyBindings[] = {
                {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            };
            VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            layoutInfo.bindingCount = std::size(copyBindings);
            layoutInfo.pBindings = copyBindings;
            if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &hiZCopyDescriptorSetLayout) != VK_SUCCESS ||
                vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &hiZReduceDescriptorSetLayout) != VK_SUCCESS) {
                throw std::runtime_error("Could not create Hi-Z descriptor-set layouts");
            }
            const VkDescriptorSetLayoutBinding cullBindings[] = {
                {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            };
            layoutInfo.bindingCount = std::size(cullBindings);
            layoutInfo.pBindings = cullBindings;
            if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &cullingDescriptorSetLayout) != VK_SUCCESS) {
                throw std::runtime_error("Could not create culling descriptor-set layout");
            }
            const auto createLayout = [&](VkDescriptorSetLayout setLayout, VkPipelineLayout& pipelineLayout) {
                VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
                info.setLayoutCount = 1; info.pSetLayouts = &setLayout;
                if (vkCreatePipelineLayout(device, &info, nullptr, &pipelineLayout) != VK_SUCCESS) {
                    throw std::runtime_error("Could not create Hi-Z pipeline layout");
                }
            };
            createLayout(hiZCopyDescriptorSetLayout, hiZCopyPipelineLayout);
            createLayout(hiZReduceDescriptorSetLayout, hiZReducePipelineLayout);
            createLayout(cullingDescriptorSetLayout, cullingPipelineLayout);
            hiZCopyPipeline = createComputePipeline("shaders/hiz_copy.comp.spv", hiZCopyPipelineLayout);
            hiZReducePipeline = createComputePipeline("shaders/hiz_reduce.comp.spv", hiZReducePipelineLayout);
            cullingPipeline = createComputePipeline("shaders/hiz_cull.comp.spv", cullingPipelineLayout);

            gpuObjects.resize(objectCount);
            for (uint32_t i = 0; i < objectCount; ++i) {
                const InstanceBatch& batch = instanceBatches[i];
                auto& object = gpuObjects[i];
                object.model = {};
                object.model.data[0] = 1.0f;
                object.model.data[5] = 1.0f;
                object.model.data[10] = 1.0f;
                object.model.data[15] = 1.0f;
                const AABB& bounds = batch.worldBounds;
                object.localAabbMin = {
                    bounds.min.x(), bounds.min.y(), bounds.min.z(), 0.0f};
                object.localAabbMax = {
                    bounds.max.x(), bounds.max.y(), bounds.max.z(), 0.0f};
                object.indexCount = batch.indexCount;
                object.instanceCount = batch.instanceCount;
                object.firstIndex = batch.firstIndex;
                object.vertexOffset = 0;
                object.firstInstance = batch.firstInstance;
                object.castShadow = batch.castShadow ? 1u : 0u;
            }
            for (Buffer& buffer : cullingObjectBuffers) {
                buffer.createHostVisible(
                    vulkanDevice.physical(), device,
                    sizeof(Culling::GPUObjectData) * gpuObjects.size(),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vulkanDevice.allocator());
                buffer.update(gpuObjects.data(),
                              sizeof(Culling::GPUObjectData) * gpuObjects.size());
            }
            // Culling buffers were initialized in full for every frame.
            for (RenderableRecord& record : renderables) {
                record.cullingDirtyFrames = 0;
            }

            for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
                cullingUniformBuffers[frame].createHostVisible(vulkanDevice.physical(), device,
                    sizeof(Culling::CullingUniformData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                    vulkanDevice.allocator());
                shadowCullingUniformBuffers[frame].createHostVisible(vulkanDevice.physical(), device,
                    sizeof(Culling::CullingUniformData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                    vulkanDevice.allocator());
                std::vector<VkDrawIndexedIndirectCommand> emptyCommands(objectCount);
                indirectBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device, emptyCommands.data(),
                    sizeof(VkDrawIndexedIndirectCommand) * objectCount,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                shadowIndirectBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device, emptyCommands.data(),
                    sizeof(VkDrawIndexedIndirectCommand) * objectCount,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                constexpr uint32_t zero = 0;
                drawCountBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device, &zero, sizeof(zero),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                shadowDrawCountBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device, &zero, sizeof(zero),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
            }

            constexpr uint32_t cullingSetCount = MAX_FRAMES_IN_FLIGHT * 2;
            const uint32_t imageDescriptors = hiZBuffer.mipCount() + cullingSetCount;
            const VkDescriptorPoolSize poolSizes[] = {
                {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, imageDescriptors},
                {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, hiZBuffer.mipCount()},
                {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cullingSetCount * 3},
                {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, cullingSetCount},
            };
            VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            poolInfo.maxSets = hiZBuffer.mipCount() + cullingSetCount;
            poolInfo.poolSizeCount = std::size(poolSizes); poolInfo.pPoolSizes = poolSizes;
            if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &cullingDescriptorPool) != VK_SUCCESS) {
                throw std::runtime_error("Could not create Hi-Z descriptor pool");
            }
            hiZPass.create(device, cullingDescriptorPool, hiZCopyPipeline, hiZCopyPipelineLayout,
                hiZCopyDescriptorSetLayout, hiZReducePipeline, hiZReducePipelineLayout,
                hiZReduceDescriptorSetLayout, hiZBuffer,
                msaa.enabled() ? hiZDepthBuffer.imageView() : depthBuffer.imageView(),
                msaa.enabled() ? hiZDepthBuffer.sampler() : depthBuffer.sampler());

            std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT * 2> cullLayouts{};
            cullLayouts.fill(cullingDescriptorSetLayout);
            std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT * 2> cullSets{};
            VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            allocateInfo.descriptorPool = cullingDescriptorPool;
            allocateInfo.descriptorSetCount = static_cast<uint32_t>(cullSets.size());
            allocateInfo.pSetLayouts = cullLayouts.data();
            if (vkAllocateDescriptorSets(device, &allocateInfo, cullSets.data()) != VK_SUCCESS) {
                throw std::runtime_error("Could not allocate culling descriptor sets");
            }
            for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
                const VkDescriptorImageInfo hiZInfo{hiZBuffer.sampler(), hiZBuffer.fullView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                const auto updateCullingSet = [&](VkDescriptorSet set, const Buffer& indirectBuffer,
                                                  const Buffer& countBuffer, const Buffer& uniformBuffer) {
                    const VkDescriptorBufferInfo objectInfo{
                        cullingObjectBuffers[frame].handle(), 0, VK_WHOLE_SIZE};
                    const VkDescriptorBufferInfo indirectInfo{indirectBuffer.handle(), 0, VK_WHOLE_SIZE};
                    const VkDescriptorBufferInfo countInfo{countBuffer.handle(), 0, sizeof(uint32_t)};
                    const VkDescriptorBufferInfo uniformInfo{uniformBuffer.handle(), 0, sizeof(Culling::CullingUniformData)};
                    VkWriteDescriptorSet writes[5]{};
                    for (uint32_t i = 0; i < 5; ++i) {
                        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                        writes[i].dstSet = set;
                        writes[i].dstBinding = i;
                        writes[i].descriptorCount = 1;
                    }
                    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[0].pBufferInfo = &objectInfo;
                    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[1].pBufferInfo = &indirectInfo;
                    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[2].pBufferInfo = &countInfo;
                    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[3].pImageInfo = &hiZInfo;
                    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; writes[4].pBufferInfo = &uniformInfo;
                    vkUpdateDescriptorSets(device, std::size(writes), writes, 0, nullptr);
                };
                VkDescriptorSet cameraCullSet = cullSets[frame];
                VkDescriptorSet shadowCullSet = cullSets[MAX_FRAMES_IN_FLIGHT + frame];
                updateCullingSet(cameraCullSet, indirectBuffers[frame], drawCountBuffers[frame],
                                 cullingUniformBuffers[frame]);
                updateCullingSet(shadowCullSet, shadowIndirectBuffers[frame], shadowDrawCountBuffers[frame],
                                 shadowCullingUniformBuffers[frame]);
                gpuCullingPasses[frame].create(device, cullingPipeline, cullingPipelineLayout, cullSets[frame],
                    indirectBuffers[frame].handle(), drawCountBuffers[frame].handle(), objectCount);
                indirectDraws[frame].create(
                    indirectBuffers[frame].handle(), drawCountBuffers[frame].handle(), objectCount);
                shadowCullingPasses[frame].create(device, cullingPipeline, cullingPipelineLayout, shadowCullSet,
                    shadowIndirectBuffers[frame].handle(), shadowDrawCountBuffers[frame].handle(), objectCount);
                shadowIndirectDraws[frame].create(
                    shadowIndirectBuffers[frame].handle(), shadowDrawCountBuffers[frame].handle(), objectCount);
            }
            hiZValid = false;
        }

        void destroyCullingResources() noexcept {
            hiZPass.destroy();
            for (auto& draw : indirectDraws) draw.destroy();
            for (auto& draw : shadowIndirectDraws) draw.destroy();
            for (Buffer& buffer : cullingUniformBuffers) buffer.destroy();
            for (Buffer& buffer : shadowCullingUniformBuffers) buffer.destroy();
            for (Buffer& buffer : indirectBuffers) buffer.destroy();
            for (Buffer& buffer : shadowIndirectBuffers) buffer.destroy();
            for (Buffer& buffer : drawCountBuffers) buffer.destroy();
            for (Buffer& buffer : shadowDrawCountBuffers) buffer.destroy();
            for (Buffer& buffer : cullingObjectBuffers) buffer.destroy();
            if (cullingDescriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, cullingDescriptorPool, nullptr);
            if (hiZCopyPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, hiZCopyPipeline, nullptr);
            if (hiZReducePipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, hiZReducePipeline, nullptr);
            if (cullingPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, cullingPipeline, nullptr);
            if (hiZCopyPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, hiZCopyPipelineLayout, nullptr);
            if (hiZReducePipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, hiZReducePipelineLayout, nullptr);
            if (cullingPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, cullingPipelineLayout, nullptr);
            if (hiZCopyDescriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, hiZCopyDescriptorSetLayout, nullptr);
            if (hiZReduceDescriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, hiZReduceDescriptorSetLayout, nullptr);
            if (cullingDescriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, cullingDescriptorSetLayout, nullptr);
            cullingDescriptorPool = VK_NULL_HANDLE; hiZCopyPipeline = hiZReducePipeline = cullingPipeline = VK_NULL_HANDLE;
            hiZCopyPipelineLayout = hiZReducePipelineLayout = cullingPipelineLayout = VK_NULL_HANDLE;
            hiZCopyDescriptorSetLayout = hiZReduceDescriptorSetLayout = cullingDescriptorSetLayout = VK_NULL_HANDLE;
            hiZBuffer.destroy(); gpuObjects.clear(); hiZValid = false;
        }

        void createFramebuffers() {
            const VkExtent2D extent = swapchain.extent();
            VkImageView msaaAttachments[] = {
                msaa.colorImageView(), depthBuffer.imageView(), hdrBuffer.imageView()
            };
            VkImageView directAttachments[] = {
                hdrBuffer.imageView(), depthBuffer.imageView()
            };

            VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            framebufferInfo.renderPass = forwardPass.renderPass();
            framebufferInfo.attachmentCount = msaa.enabled() ? 3u : 2u;
            framebufferInfo.pAttachments = msaa.enabled()
                ? msaaAttachments
                : directAttachments;
            framebufferInfo.width = extent.width;
            framebufferInfo.height = extent.height;
            framebufferInfo.layers = 1;

            if (vkCreateFramebuffer(device, &framebufferInfo, nullptr,
                                    &hdrFramebuffer) != VK_SUCCESS) {
                throw std::runtime_error("Could not create HDR framebuffer");
            }

            if (msaa.enabled()) {
                const VkImageView prepassAttachments[] = {
                    hdrBuffer.imageView(), hiZDepthBuffer.imageView()};
                VkFramebufferCreateInfo prepassInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
                prepassInfo.renderPass = hiZDepthPrepass.renderPass();
                prepassInfo.attachmentCount = 2;
                prepassInfo.pAttachments = prepassAttachments;
                prepassInfo.width = extent.width;
                prepassInfo.height = extent.height;
                prepassInfo.layers = 1;
                if (vkCreateFramebuffer(device, &prepassInfo, nullptr,
                                        &hiZDepthPrepassFramebuffer) != VK_SUCCESS) {
                    throw std::runtime_error("Could not create Hi-Z depth prepass framebuffer");
                }
            }
        }

        void createSceneViewportResources() {
            // Start with the drawable extent. The editor will later supply its
            // panel extent through the renderer viewport API; creating it here
            // also makes the off-screen lifecycle valid for non-editor users.
            sceneViewportTarget.create(vulkanDevice.physical(), device, swapchain.extent(),
                                       msaa.sampleCount());
            // The forward render pass uses the same MSAA attachment layout for
            // Game View and Scene View: multisampled color, multisampled depth,
            // then a single-sample resolve target. The Scene View used to bind
            // only its single-sample color and depth images here, which made
            // the framebuffer incompatible as soon as MSAA was enabled.
            VkImageView msaaAttachments[] = {
                sceneViewportTarget.msaaColorImageView(), sceneViewportTarget.depth().imageView(),
                sceneViewportTarget.color().imageView()};
            VkImageView directAttachments[] = {
                sceneViewportTarget.color().imageView(), sceneViewportTarget.depth().imageView()};
            VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            info.renderPass = forwardPass.renderPass();
            info.attachmentCount = msaa.enabled() ? 3u : 2u;
            info.pAttachments = msaa.enabled() ? msaaAttachments : directAttachments;
            info.width = sceneViewportTarget.extent().width;
            info.height = sceneViewportTarget.extent().height;
            info.layers = 1;
            if (vkCreateFramebuffer(device, &info, nullptr, &sceneViewportFramebuffer) != VK_SUCCESS) {
                sceneViewportTarget.destroy();
                throw std::runtime_error("Could not create Scene View framebuffer");
            }
        }

        void destroySceneViewportResources() noexcept {
            if (sceneViewportFramebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device, sceneViewportFramebuffer, nullptr);
                sceneViewportFramebuffer = VK_NULL_HANDLE;
            }
            sceneViewportTarget.destroy();
        }

        void createCommandPool() {
            VkCommandPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            poolInfo.queueFamilyIndex = vulkanDevice.graphicsQueueFamily();

            if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
                throw std::runtime_error("Could not create command pool");
            }
        }

        void createCommandBuffers() {
            commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);

            VkCommandBufferAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocInfo.commandPool = commandPool;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers.size());

            if (vkAllocateCommandBuffers(device, &allocInfo, commandBuffers.data()) != VK_SUCCESS) {
                throw std::runtime_error("Could not allocate command buffers");
            }
        }

        Mat4 lightSpaceMatrix() const {
            const auto light = directionalLight();
            const float extent = sceneRadius;
            const Vec3 lightTarget = sceneCenter;
            const Vec3 lightPosition = lightTarget - light.direction * (extent * 5.0f);
            constexpr Vec3 worldUp{0.0f, 1.0f, 0.0f};
            const Mat4 lightView = Mat4::lookAt(lightPosition, lightTarget, worldUp);
            const Mat4 lightProjection = Mat4::scale(
                Mat4::ortho(-extent * 2.25f, extent * 2.25f,
                            -extent * 2.25f, extent * 2.25f,
                            0.1f, extent * 8.0f),
                Vec3{1.0f, -1.0f, 1.0f});
            return lightProjection * lightView;
        }

        struct DirectionalLight final {
            Vec3 direction{-0.45f, -0.80f, -0.35f};
            Math::Color color = Math::Color::white();
            float intensity{4.0f};
        };

        [[nodiscard]] DirectionalLight directionalLight() const {
            DirectionalLight result;
            const Registry& readRegistry = registry;
            bool found = false;
            readRegistry.view<Transform, LightComponent>(
                [&](const Entity, const Transform& transform, const LightComponent& light) {
                    if (found || !light.enabled || light.type != LightType::Directional) return;
                    const glm::vec3 direction = glm::vec3(transform.matrix().native() *
                                                          glm::vec4{0.0f, 0.0f, -1.0f, 0.0f});
                    if (glm::length(direction) <= 1e-6f) return;
                    result.direction = Vec3{glm::normalize(direction)};
                    result.color = light.color;
                    result.intensity = std::max(0.0f, light.intensity);
                    found = true;
                });
            return result;
        }

        void updateUniformBuffer(const uint32_t frame) {
            Entity activeCamera = NullEntity;
            const Registry& readRegistry = registry;
            readRegistry.view<CameraComponent, Transform>(
                [&](const Entity entity, const CameraComponent& component, const Transform&) {
                    if (activeCamera == NullEntity && component.primary) {
                        activeCamera = entity;
                    }
                });
            if (activeCamera == NullEntity) {
                throw std::runtime_error("Scene has no primary CameraComponent with Transform");
            }

            const CameraComponent& component = readRegistry.get<CameraComponent>(activeCamera);
            const Transform& transform = readRegistry.get<Transform>(activeCamera);
            if (!component.isPerspective() || !component.isValid()) {
                throw std::runtime_error("Primary CameraComponent has unsupported settings");
            }

            // Game View is presented in a fixed 16:9 editor frame. Keep the
            // projection in that aspect too, independently of dock layout.
            const float gameAspect = editorUiActive ? (16.0f / 9.0f) : component.aspectRatio;
            camera.emplace(Degrees{component.fieldOfView}, gameAspect,
                           component.nearClip, component.farClip);
            camera->setPosition(transform.position);
            camera->setRotation(Degrees{transform.rotation.y()},
                                Degrees{transform.rotation.x()});

            const DirectionalLight light = directionalLight();
            const UniformBufferObject data{
                camera->viewMatrix(), camera->projectionMatrix(), lightSpaceMatrix(),
                glm::vec4{camera->position().native(), 1.0f},
                glm::vec4{light.direction.native(), light.intensity},
                glm::vec4{light.color.r(), light.color.g(), light.color.b(), 1.0f},
                (optimizationFeatures.shadows && hasShadowCasters) ? 1u : 0u,
                materialSlots, editorSelectedRenderable};
            uniformBuffers[frame].update(&data, sizeof(data));
        }

        void updateSceneViewportUniformBuffer(const uint32_t frame) {
            const float aspect = static_cast<float>(sceneViewportTarget.extent().width) /
                                 static_cast<float>(sceneViewportTarget.extent().height);
            Camera sceneCamera{Degrees{60.0f}, aspect, 0.1f, 1000.0f};
            sceneCamera.setPosition(editorSceneCameraPosition);
            sceneCamera.setRotation(Degrees{editorSceneCameraYaw},
                                    Degrees{editorSceneCameraPitch});
            const DirectionalLight light = directionalLight();
            const UniformBufferObject data{
                sceneCamera.viewMatrix(), sceneCamera.projectionMatrix(), lightSpaceMatrix(),
                glm::vec4{sceneCamera.position().native(), 1.0f},
                glm::vec4{light.direction.native(), light.intensity},
                glm::vec4{light.color.r(), light.color.g(), light.color.b(), 1.0f},
                0u, materialSlots, editorSelectedRenderable};
            sceneUniformBuffers[frame].update(&data, sizeof(data));
        }

        void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
            VkCommandBufferBeginInfo beginInfo{};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

            if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
                throw std::runtime_error("Could not begin command buffer");
            }
            // Culling runs before this frame's depth pass, so it consumes the
            // Hi-Z result from the previous frame. On the first frame there is
            // no previous result, but the descriptor is still bound and the
            // image must be in the layout declared in that descriptor. The
            // culling uniform's cameraCut flag disables occlusion testing for
            // this frame, so an undefined image contents is acceptable after
            // this layout transition.
            const bool hizEnabled = canUseHiZOcclusionCulling();
            const bool hadPreviousHiZ = hiZValid;
            // The culling descriptor set always contains the Hi-Z image. Keep
            // its layout valid before the compute culling dispatch, even when
            // that dispatch skips occlusion testing.
            if (optimizationFeatures.gpuCulling && !hadPreviousHiZ) {
                VkImageMemoryBarrier2 initialBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                initialBarrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
                initialBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                initialBarrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                initialBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                initialBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                initialBarrier.image = hiZBuffer.image();
                initialBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                                   hiZBuffer.mipCount(), 0, 1};

                VkDependencyInfo initialDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                initialDependency.imageMemoryBarrierCount = 1;
                initialDependency.pImageMemoryBarriers = &initialBarrier;
                vkCmdPipelineBarrier2(commandBuffer, &initialDependency);
            }

            if (particleSystem) {
                particleSystem->recordCompute(commandBuffer, particleComputePipeline,
                                              particleComputePipelineLayout, currentFrame);
            }

            // The forward descriptor layout always contains the shadow-map
            // sampler. Even when shadows are disabled, run an empty shadow
            // pass so its image is transitioned from UNDEFINED to
            // SHADER_READ_ONLY_OPTIMAL before the descriptor is used.
            shadowPass.record(
                commandBuffer, lightSpaceMatrix(), vertexBuffer.handle(),
                shadowInstanceBuffers[currentFrame].handle(), indexBuffer.handle(),
                shadowPass.descriptorSet(currentFrame),
                shadowCullingPasses[currentFrame],
                shadowIndirectDraws[currentFrame],
                optimizationFeatures.shadows
                    ? static_cast<std::uint32_t>(gpuObjects.size())
                    : 0u);

            gpuCullingPasses[currentFrame].record(
                commandBuffer, static_cast<std::uint32_t>(gpuObjects.size()));

            // Never sample or resolve the multisampled depth attachment for
            // Hi-Z.  A separate 1x prepass gives the hierarchy a conventional
            // sampler2D source, then the main forward pass can use MSAA solely
            // for the visible rasterization.
            if (hizEnabled && msaa.enabled()) {
                hiZDepthPrepass.begin(
                    commandBuffer, hiZDepthPrepassFramebuffer, swapchain.extent(),
                    shadowPass.descriptorSet(currentFrame), vertexBuffer.handle(),
                    instanceBuffers[currentFrame].handle(), indexBuffer.handle());
                hiZDepthPrepass.draw(commandBuffer, indirectDraws[currentFrame]);
                hiZDepthPrepass.end(commandBuffer);

                VkImageMemoryBarrier2 depthReady{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                depthReady.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
                depthReady.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                depthReady.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                depthReady.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                // ForwardPass's render pass transitions the depth attachment
                // to READ_ONLY on exit, including its first use. Keep the
                // layout unchanged here and only add the visibility barrier.
                depthReady.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                depthReady.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                depthReady.image = hiZDepthBuffer.image();
                depthReady.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
                VkDependencyInfo depthDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                depthDependency.imageMemoryBarrierCount = 1;
                depthDependency.pImageMemoryBarriers = &depthReady;
                vkCmdPipelineBarrier2(commandBuffer, &depthDependency);
                hiZPass.record(commandBuffer, hiZBuffer, true);
                hiZValid = true;
            }

            forwardPass.begin(
                commandBuffer, hdrFramebuffer, swapchain.extent(),
                shadowPass.descriptorSet(currentFrame), vertexBuffer.handle(),
                instanceBuffers[currentFrame].handle(), indexBuffer.handle());
            forwardPass.draw(commandBuffer, indirectDraws[currentFrame]);
            // Fill the background before drawing particles. Otherwise the
            // skybox can overwrite transparent particle fragments in the sky.
            skyPass.record(commandBuffer, currentFrame);
            if (particleSystem && camera) {
                const Particles::ParticleFrameData particleFrame{
                    camera->projectionMatrix() * camera->viewMatrix(),
                    camera->right(),
                    0.0f,
                    camera->up(),
                    0.0f,
                };
                particleSystem->recordRender(commandBuffer, particleFrame,
                                             particlePipeline.handle(), particlePipeline.layout(),
                                              currentFrame, false);
            }
            forwardPass.end(commandBuffer);

            // Render the scene into an off-screen image with the very same
            // scene data and draw infrastructure as Game View. At this stage
            // the scene camera descriptor is still wired in the following
            // change; keeping the pass here gives the target a real render
            // lifecycle immediately instead of merely clearing an image.
            forwardPass.begin(
                commandBuffer, sceneViewportFramebuffer, sceneViewportTarget.extent(),
                sceneDescriptorPass.descriptorSet(currentFrame), vertexBuffer.handle(),
                instanceBuffers[currentFrame].handle(), indexBuffer.handle());
            forwardPass.draw(commandBuffer, indirectDraws[currentFrame]);
            sceneSkyPass.record(commandBuffer, currentFrame);
            if (particleSystem) {
                Camera sceneCamera{Degrees{60.0f},
                                   static_cast<float>(sceneViewportTarget.extent().width) /
                                       static_cast<float>(sceneViewportTarget.extent().height),
                                   0.1f, 1000.0f};
                sceneCamera.setPosition(editorSceneCameraPosition);
                sceneCamera.setRotation(Degrees{editorSceneCameraYaw},
                                        Degrees{editorSceneCameraPitch});
                const Particles::ParticleFrameData particleFrame{
                    sceneCamera.projectionMatrix() * sceneCamera.viewMatrix(),
                    sceneCamera.right(),
                    0.0f,
                    sceneCamera.up(),
                    0.0f,
                };
                particleSystem->recordRender(commandBuffer, particleFrame,
                                             particlePipeline.handle(), particlePipeline.layout(),
                                              currentFrame, true);
            }
            forwardPass.end(commandBuffer);

            if (hizEnabled && !msaa.enabled()) {
                VkImageMemoryBarrier2 depthReady{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                depthReady.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
                depthReady.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                depthReady.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                depthReady.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                // The forward render pass already leaves depth in READ_ONLY.
                // This barrier supplies visibility for the following compute
                // pass without inventing an UNDEFINED transition.
                depthReady.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                depthReady.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                depthReady.image = depthBuffer.image();
                depthReady.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
                VkDependencyInfo depthDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                depthDependency.imageMemoryBarrierCount = 1;
                depthDependency.pImageMemoryBarriers = &depthReady;
                vkCmdPipelineBarrier2(commandBuffer, &depthDependency);
                // The first-frame initialization above establishes a valid image
                // layout, so HiZPass must transition from SHADER_READ_ONLY rather
                // than from UNDEFINED when it builds the first hierarchy.
                hiZPass.record(commandBuffer, hiZBuffer, true);
                hiZValid = true;
            }

            if (editorUiActive) {
                VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
                pass.renderPass = editorUiRenderPass;
                pass.framebuffer = editorUiFramebuffers.at(imageIndex);
                pass.renderArea.extent = swapchain.extent();
                VkClearValue clear{};
                clear.color = {{0.06f, 0.07f, 0.09f, 1.0f}};
                pass.clearValueCount = 1;
                pass.pClearValues = &clear;
                vkCmdBeginRenderPass(commandBuffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
                ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
                vkCmdEndRenderPass(commandBuffer);
            } else {
                tonemapPass.record(commandBuffer, imageIndex, swapchain.extent());
                canvasRenderer.record(scene.uiCanvas(), commandBuffer, imageIndex, currentFrame,
                                      swapchain.extent());
            }

            if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
                throw std::runtime_error("Could not end command buffer");
            }
        }

        void createSyncObjects() {
            imageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
            renderFinishedSemaphores.resize(swapchain.imageCount());
            inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);

            VkSemaphoreCreateInfo semaphoreInfo{};
            semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

            for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
                if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &imageAvailableSemaphores[i]) != VK_SUCCESS ||
                    vkCreateFence(device, &fenceInfo, nullptr, &inFlightFences[i]) != VK_SUCCESS) {
                    throw std::runtime_error("Could not create synchronization objects");
                    }
            }

            createRenderFinishedSemaphores();
        }

        // ---------- SWAPCHAIN RECREATE ----------

        void cleanupSwapChain() {
            destroyEditorUiResources();
            canvasRenderer.destroy();
            tonemapPass.destroy();
            if (hiZDepthPrepassFramebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device, hiZDepthPrepassFramebuffer, nullptr);
                hiZDepthPrepassFramebuffer = VK_NULL_HANDLE;
            }
            if (hdrFramebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device, hdrFramebuffer, nullptr);
                hdrFramebuffer = VK_NULL_HANDLE;
            }
            destroySceneViewportResources();

            msaa.destroy();
            hdrBuffer.destroy();
            destroyDepthResources();
            destroyRenderFinishedSemaphores();
            swapchain.destroy();
        }

        void destroyRenderFinishedSemaphores() noexcept {
            for (VkSemaphore semaphore : renderFinishedSemaphores) {
                if (semaphore != VK_NULL_HANDLE) {
                    vkDestroySemaphore(device, semaphore, nullptr);
                }
            }
            renderFinishedSemaphores.clear();
        }

        void createRenderFinishedSemaphores() {
            renderFinishedSemaphores.resize(swapchain.imageCount());

            VkSemaphoreCreateInfo semaphoreInfo{};
            semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            for (VkSemaphore& semaphore : renderFinishedSemaphores) {
                if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore) != VK_SUCCESS) {
                    destroyRenderFinishedSemaphores();
                    throw std::runtime_error("Could not create render-finished semaphore");
                }
            }
        }

        void recreateSwapChain() {
            waitForDrawableExtent();

            vkDeviceWaitIdle(device);

            // The ImGui Vulkan backend owns swapchain-dependent render data.
            // Tear down both ImGui backends before recreating the presentation
            // resources, then register the SDL window again below.
            destroyEditorUiResources();

            if (hiZDepthPrepassFramebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device, hiZDepthPrepassFramebuffer, nullptr);
                hiZDepthPrepassFramebuffer = VK_NULL_HANDLE;
            }
            destroyCullingResources();

            canvasRenderer.destroy();
            tonemapPass.destroy();
            if (hdrFramebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device, hdrFramebuffer, nullptr);
                hdrFramebuffer = VK_NULL_HANDLE;
            }
            destroySceneViewportResources();
            msaa.destroy();
            hdrBuffer.destroy();
            destroyDepthResources();
            destroyRenderFinishedSemaphores();

            swapchain.recreate();
            registry.view<CameraComponent>([&](const Entity, CameraComponent& component) {
                if (component.primary) {
                    component.setAspectRatio(static_cast<float>(swapchain.extent().width),
                                             static_cast<float>(swapchain.extent().height));
                }
            });
            hdrBuffer.create(vulkanDevice.physical(), device, swapchain.extent());
            msaa.create(swapchain.extent(), HdrBuffer::Format);
            createDepthResources();
            createRenderFinishedSemaphores();

            createFramebuffers();
            createSceneViewportResources();
            createTonemapPass();
            createUIResources();
            createEditorUiResources();
            createCullingResources();
        }

        // ---------- MAIN LOOP ----------

        void updateCameraInput() {
            if (editorUiActive) {
                if (editorSceneCameraInput) {
                    updateEditorSceneCameraInput();
                } else if (cameraMouseLookActive) {
                    SDLInput::setRelativeMouseMode(window, false);
                    cameraMouseLookActive = false;
                }
                return;
            }

            CameraComponent* activeCameraComponent = nullptr;
            Transform* activeCameraTransform = nullptr;
            registry.view<CameraComponent, Transform>(
                [&](const Entity, CameraComponent& component, Transform& transform) {
                    if (activeCameraComponent == nullptr && component.primary) {
                        activeCameraComponent = &component;
                        activeCameraTransform = &transform;
                    }
                });

            if (activeCameraComponent == nullptr) {
                if (cameraMouseLookActive) {
                    SDLInput::setRelativeMouseMode(window, false);
                    cameraMouseLookActive = false;
                }
                return;
            }

            // Do not call Registry::get here: its mutable overload advances the
            // global scene revision. Camera movement changes only view uniforms,
            // not any renderable instance data; advancing that revision made
            // updateRenderableBuffers scan all 27,000 cubes every frame.
            const CameraComponent& cameraComponent = *activeCameraComponent;
            Transform& transform = *activeCameraTransform;
            Camera controlledCamera(
                Degrees{cameraComponent.fieldOfView}, cameraComponent.aspectRatio,
                cameraComponent.nearClip, cameraComponent.farClip);
            controlledCamera.setRotation(Degrees{transform.rotation.y()},
                                          Degrees{transform.rotation.x()});

            Vec3 movement{};
            const Vec3 forward = controlledCamera.forward();
            const Vec3 horizontalForward{forward.x(), 0.0f, forward.z()};
            if (horizontalForward.length() > 0.0f) {
                const Vec3 flatForward = horizontalForward.normalized();
                if (Input::keyDown(KeyCode::W)) movement += flatForward;
                if (Input::keyDown(KeyCode::S)) movement -= flatForward;
            }
            const Vec3 right = controlledCamera.right();
            if (Input::keyDown(KeyCode::D)) movement += right;
            if (Input::keyDown(KeyCode::A)) movement -= right;

            constexpr float MOVE_SPEED = 10.0f;
            if (movement.length() > 0.0f) {
                transform.position += movement.normalized() *
                    (MOVE_SPEED * static_cast<float>(Time::deltaTime()));
            }

            constexpr float ZOOM_SPEED = 2.0f;
            const float wheel = Input::mouseWheel();
            if (wheel != 0.0f) {
                transform.position += controlledCamera.forward() * (wheel * ZOOM_SPEED);
            }

            constexpr float MOUSE_SENSITIVITY = 0.1f;
            if (Input::mouseDown(MouseButton::Right)) {
                if (!cameraMouseLookActive) {
                    SDLInput::setRelativeMouseMode(window, true);
                    cameraMouseLookActive = true;
                }

                const Vec2 mouseDelta = Input::mouseDelta();
                transform.rotation.setY(transform.rotation.y() +
                                        mouseDelta.x() * MOUSE_SENSITIVITY);
                transform.rotation.setX(std::clamp(
                    transform.rotation.x() - mouseDelta.y() * MOUSE_SENSITIVITY,
                    -89.0f, 89.0f));
            } else if (cameraMouseLookActive) {
                SDLInput::setRelativeMouseMode(window, false);
                cameraMouseLookActive = false;
            }
        }

        void updateEditorSceneCameraInput() {
            // Relative mouse mode belongs to gameplay. In the editor it can
            // keep SDL's pointer capture after a Scene View drag, preventing
            // Dear ImGui controls from receiving later clicks. Keep normal
            // mouse coordinates here; right-drag still supplies deltas for
            // camera rotation without taking ownership of the cursor.
            if (cameraMouseLookActive) {
                SDLInput::setRelativeMouseMode(window, false);
                cameraMouseLookActive = false;
            }

            Camera sceneCamera{Degrees{60.0f}, 1.0f, 0.1f, 1000.0f};
            sceneCamera.setRotation(Degrees{editorSceneCameraYaw},
                                    Degrees{editorSceneCameraPitch});

            Vec3 movement{};
            const Vec3 forward = sceneCamera.forward();
            const Vec3 horizontalForward{forward.x(), 0.0f, forward.z()};
            if (horizontalForward.length() > 0.0f) {
                const Vec3 flatForward = horizontalForward.normalized();
                if (Input::keyDown(KeyCode::W)) movement += flatForward;
                if (Input::keyDown(KeyCode::S)) movement -= flatForward;
            }
            if (Input::keyDown(KeyCode::D)) movement += sceneCamera.right();
            if (Input::keyDown(KeyCode::A)) movement -= sceneCamera.right();

            if (Input::mouseDown(MouseButton::Middle)) {
                const Vec2 mouseDelta = Input::mouseDelta();
                constexpr float panSensitivity = 0.03f;
                editorSceneCameraPosition -= sceneCamera.right() *
                    (mouseDelta.x() * panSensitivity);
                editorSceneCameraPosition += sceneCamera.up() *
                    (mouseDelta.y() * panSensitivity);
            }

            constexpr float moveSpeed = 10.0f;
            if (movement.length() > 0.0f) {
                editorSceneCameraPosition += movement.normalized() *
                    (moveSpeed * static_cast<float>(Time::deltaTime()));
            }

            constexpr float zoomSpeed = 2.0f;
            editorSceneCameraPosition += sceneCamera.forward() *
                (Input::mouseWheel() * zoomSpeed);

            constexpr float mouseSensitivity = 0.1f;
            if (Input::mouseDown(MouseButton::Right)) {
                const Vec2 mouseDelta = Input::mouseDelta();
                editorSceneCameraYaw += mouseDelta.x() * mouseSensitivity;
                editorSceneCameraPitch = std::clamp(
                    editorSceneCameraPitch - mouseDelta.y() * mouseSensitivity,
                    -89.0f, 89.0f);
            }
        }

        void drawFrame() {
            vkWaitForFences(device, 1, &inFlightFences[currentFrame], VK_TRUE, UINT64_MAX);
            uint32_t imageIndex;
            VkResult result = vkAcquireNextImageKHR(device, swapchain.handle(), UINT64_MAX,
                imageAvailableSemaphores[currentFrame], VK_NULL_HANDLE, &imageIndex);

            if (result == VK_ERROR_OUT_OF_DATE_KHR) {
                recreateSwapChain();
                return;
            } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
                throw std::runtime_error("Could not acquire swap chain image");
            }

            vkResetFences(device, 1, &inFlightFences[currentFrame]);

            vkResetCommandBuffer(commandBuffers[currentFrame], 0);
            updateUniformBuffer(currentFrame);
            updateSceneViewportUniformBuffer(currentFrame);
            updateRenderableBuffers();
            if (particleSystem) {
                particleSystem->update(static_cast<float>(Time::deltaTime()));
            }
            updateCullingUniformBuffer(currentFrame);
            if (optimizationFeatures.shadows) {
                updateShadowCullingUniformBuffer(currentFrame);
            }
            recordCommandBuffer(commandBuffers[currentFrame], imageIndex);

            VkSubmitInfo submitInfo{};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

            VkSemaphore waitSemaphores[] = {imageAvailableSemaphores[currentFrame]};
            VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
            submitInfo.waitSemaphoreCount = 1;
            submitInfo.pWaitSemaphores = waitSemaphores;
            submitInfo.pWaitDstStageMask = waitStages;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &commandBuffers[currentFrame];

            VkSemaphore signalSemaphores[] = {renderFinishedSemaphores[imageIndex]};
            submitInfo.signalSemaphoreCount = 1;
            submitInfo.pSignalSemaphores = signalSemaphores;

            const VkResult submitResult = vkQueueSubmit(
                vulkanDevice.graphicsQueue(), 1, &submitInfo, inFlightFences[currentFrame]);
            if (submitResult != VK_SUCCESS) {
                throw std::runtime_error(
                    "Could not submit command buffer to queue (VkResult " +
                    std::to_string(static_cast<int>(submitResult)) + ")");
            }
            VkPresentInfoKHR presentInfo{};
            presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            presentInfo.waitSemaphoreCount = 1;
            presentInfo.pWaitSemaphores = signalSemaphores;

            VkSwapchainKHR swapChains[] = {swapchain.handle()};
            presentInfo.swapchainCount = 1;
            presentInfo.pSwapchains = swapChains;
            presentInfo.pImageIndices = &imageIndex;

            result = vkQueuePresentKHR(vulkanDevice.presentQueue(), &presentInfo);

            if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || framebufferResized) {
                framebufferResized = false;
                recreateSwapChain();
            } else if (result != VK_SUCCESS) {
                throw std::runtime_error("Could not present image");
            }

            currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
        }

        void updateFpsCounter() {
            fpsFrameCount++;
            fpsElapsedTime += Time::unscaledDeltaTime();

            if (fpsElapsedTime >= 1.0) {
                const double fps = fpsFrameCount / fpsElapsedTime;

                char title[128];
                snprintf(title, sizeof(title),
                         "GamEngine | FPS: %.1f | Renderables: %zu",
                         fps, renderables.size());
                SDL_SetWindowTitle(window, title);

                fpsFrameCount = 0;
                fpsElapsedTime = 0.0;
                scene.setFps(fps);
            }
        }

        // ---------- CLEANUP ----------

        void cleanup() {
            if (cleanedUp) {
                return;
            }
            cleanedUp = true;

            if (device != VK_NULL_HANDLE) {
                vkDeviceWaitIdle(device);
                cleanupSwapChain();

                skyPass.destroy();
                sceneSkyPass.destroy();
                particlePipeline.destroy();
                vkDestroyPipeline(device, particleComputePipeline, nullptr);
                vkDestroyPipelineLayout(device, particleComputePipelineLayout, nullptr);
                particleComputePipeline = VK_NULL_HANDLE;
                particleComputePipelineLayout = VK_NULL_HANDLE;
                particleSystem.reset();
                forwardPass.destroy();
                hiZDepthPrepass.destroy();
                shadowPass.destroy();
                sceneDescriptorPass.destroy();
                destroyCullingResources();
                indexBuffer.destroy();
                vertexBuffer.destroy();
                for (Buffer& buffer : instanceBuffers) {
                    buffer.destroy();
                }
                for (Buffer& buffer : shadowInstanceBuffers) {
                    buffer.destroy();
                }
                for (Buffer& buffer : materialBuffers) {
                    buffer.destroy();
                }
                for (Buffer& uniformBuffer : uniformBuffers) {
                    uniformBuffer.destroy();
                }
                for (Buffer& uniformBuffer : sceneUniformBuffers) {
                    uniformBuffer.destroy();
                }
                fpsFontTexture.destroy();
                for (Texture2D& texture : materialTextures) {
                    texture.destroy();
                }
                materialTextures.clear();
                materialTextureDescriptors.clear();
                meshTextureOffsets.clear();
                fallbackMaterialTexture.destroy();

                for (VkSemaphore semaphore : imageAvailableSemaphores) {
                    if (semaphore != VK_NULL_HANDLE) {
                        vkDestroySemaphore(device, semaphore, nullptr);
                    }
                }
                for (VkFence fence : inFlightFences) {
                    if (fence != VK_NULL_HANDLE) {
                        vkDestroyFence(device, fence, nullptr);
                    }
                }

                imageAvailableSemaphores.clear();
                inFlightFences.clear();

                if (commandPool != VK_NULL_HANDLE) {
                    vkDestroyCommandPool(device, commandPool, nullptr);
                    commandPool = VK_NULL_HANDLE;
                }

                vulkanDevice.destroy();
                device = VK_NULL_HANDLE;
            }

            if (instance != VK_NULL_HANDLE && debugMessenger != VK_NULL_HANDLE) {
                DestroyDebugUtilsMessengerEXT(instance, debugMessenger, nullptr);
                debugMessenger = VK_NULL_HANDLE;
            }

            if (instance != VK_NULL_HANDLE && surface != VK_NULL_HANDLE) {
                vkDestroySurfaceKHR(instance, surface, nullptr);
                surface = VK_NULL_HANDLE;
            }
            if (instance != VK_NULL_HANDLE) {
                vkDestroyInstance(instance, nullptr);
                instance = VK_NULL_HANDLE;
            }

            // The application, not the renderer, owns the SDL window and SDL lifetime.
            window = nullptr;
        }
    };

Renderer::~Renderer() = default;
Renderer::Renderer(RenderOptimizationFeatures features)
    : optimizationFeatures_(features) {}

void Renderer::initialize(Scene& scene, SDL_Window* window) {
    if (backend_) throw std::logic_error("Renderer is already initialized");
    backend_ = std::make_unique<Backend>(scene, window, optimizationFeatures_, antialiasingLevel_, assetManager_,
                                         forwardPass_, skyPass_, tonemapPass_, particlePipeline_,
                                         canvasRenderer_);
    backend_->initialize();
}

void Renderer::beginFrame() { backend_->beginFrame(); }
void Renderer::beginEditorUiFrame() { backend_->beginEditorUiFrame(); }
void Renderer::processEvent(const SDL_Event& event) { backend_->processEvent(event); }

void Renderer::setEditorSceneCameraInput(const bool active) {
    if (backend_) backend_->setEditorSceneCameraInput(active);
}
void Renderer::setEditorSelection(const Entity entity) {
    if (backend_) backend_->setEditorSelection(entity);
}
void Renderer::renderFrame() { backend_->renderFrame(); }
void Renderer::reloadScene(Scene& scene, SDL_Window* window) {
    // The scene buffers and synchronization objects may still be referenced by
    // the just-submitted frame. Backend cleanup also waits, but doing it here
    // makes the lifetime boundary explicit before tearing down Vulkan state.
    if (backend_) backend_->waitIdle();
    backend_.reset();
    initialize(scene, window);
}
void Renderer::reconfigureAntialiasing() {
    if (!backend_) return;
    backend_->reconfigureAntialiasing();
}
VkDescriptorSet Renderer::gameViewportDescriptor() const noexcept {
    return backend_ ? backend_->gameViewportTexture() : VK_NULL_HANDLE;
}
VkDescriptorSet Renderer::sceneViewportDescriptor() const noexcept {
    return backend_ ? backend_->sceneViewportTexture() : VK_NULL_HANDLE;
}
void Renderer::shutdown() noexcept {
    backend_.reset();
    if (ImGui::GetCurrentContext() != nullptr &&
        ImGui::GetIO().BackendPlatformUserData != nullptr) {
        ImGui_ImplSDL3_Shutdown();
    }
}

} // namespace Engine
