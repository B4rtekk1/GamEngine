#include "Engine/Renderer/Vulkan/renderer.h"

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include "Engine/Renderer/Vulkan/msaa.h"
#include "Engine/Renderer/Vulkan/depth_buffer.h"
#include "Engine/Renderer/Vulkan/hdr_buffer.h"
#include "Engine/Renderer/shader_loader.h"
#include "Engine/Renderer/Vulkan/buffer.h"
#include "Engine/Renderer/Vulkan/vulkan_device.h"
#include "Engine/Renderer/Vulkan/swapchain.h"
#include "Engine/Renderer/Geometry/Vertex.h"
#include "Engine/Renderer/Geometry/Mesh.h"
#include "Engine/ECS/Registry.h"
#include "Engine/ECS/Components/CameraComponent.h"
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
#include "Engine/Input/Input.h"
#include "Engine/UI/Canvas.h"
#include "Engine/UI/CanvasRenderer.h"
#include "Engine/UI/PanelElement.h"
#include "Platform/SDL/SDLInput.h"

#include <cstdint>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <filesystem>
#include <optional>

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

namespace {
    class RenderApp {
    public:
        explicit RenderApp(Registry& registry) : registry(registry) {}

        ~RenderApp() {
            cleanup();
        }

        void run() {
            initWindow();
            initVulkan();
            mainLoop();
            cleanup();
        }

    private:
        SDL_Window* window = nullptr;

        VkInstance instance{};
        VkDebugUtilsMessengerEXT debugMessenger{};
        VkSurfaceKHR surface{};

        VulkanDevice vulkanDevice;
        VkDevice device = VK_NULL_HANDLE;

        Swapchain swapchain;
        VkFramebuffer hdrFramebuffer = VK_NULL_HANDLE;

        MsaaResources msaa;
        HdrBuffer hdrBuffer;

        ForwardPass forwardPass;
        SkyPass skyPass;
        TonemapPass tonemapPass;
        UI::Canvas canvas{WIDTH, HEIGHT};
        UI::CanvasRenderer canvasRenderer;
        DepthBuffer depthBuffer;
        ShadowPass shadowPass;
        Registry& registry;
        Assets::AssetManager assetManager;
        std::optional<Camera> camera;
        Buffer vertexBuffer;
        Buffer indexBuffer;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> instanceBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowInstanceBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> materialBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> uniformBuffers;
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
        struct RenderableRecord {
            Entity entity{NullEntity};
            AABB localBounds{};
        };
        std::vector<RenderableRecord> renderables;
        std::vector<glm::mat4> instanceModels;
        std::vector<glm::mat4> shadowInstanceModels;
        std::vector<GPUMaterialData> materials;
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


        void initWindow() {
            if (!SDL_Init(SDL_INIT_VIDEO)) {
                throw std::runtime_error(std::string("SDL_Init error: ") + SDL_GetError());
            }
            window = SDL_CreateWindow(
            "Vulkan + SDL3 - Cube",
                WIDTH, HEIGHT,
                SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE
            );
            if (!window) {
                throw std::runtime_error(std::string("SDL_CreateWindow error: ") + SDL_GetError());
            }
        }

        void initVulkan() {
            const char* basePath = SDL_GetBasePath();
            assetManager.set_asset_root(basePath ? std::filesystem::path(basePath) : std::filesystem::path{});
            Assets::register_default_asset_loaders(assetManager);
            assetManager.set_error_handler([](const std::string& message) { std::cerr << "[Assets] " << message << '\\n'; });
            createInstance();
            setupDebugMessenger();
            createSurface();
            vulkanDevice.create(instance, surface);
            device = vulkanDevice.logical();
            depthBuffer.initialize(vulkanDevice.physical(), device);
            // Hi-Z samples the completed depth attachment directly; a single-sample
            // depth buffer keeps that path portable without a depth-resolve pass.
            msaa.initialize(vulkanDevice.physical(), device, VK_SAMPLE_COUNT_1_BIT);
            createSwapChain();
            hdrBuffer.create(vulkanDevice.physical(), device, swapchain.extent());
            msaa.create(swapchain.extent(), HdrBuffer::Format);
            createDepthResources();
            createCommandPool();
            createMeshBuffers();
            createInstanceBuffer();
            createUniformBuffers();
            createShadowPass();
            createForwardPass();
            createCullingResources();
            createSkyPass();
            createFramebuffers();
            createTonemapPass();
            createUIResources();
            createCommandBuffers();
            createSyncObjects();
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
            appInfo.apiVersion = VK_API_VERSION_1_4;

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


    void createDepthResources() {
        depthBuffer.create(swapchain.extent(), msaa.sampleCount());
    }

    void destroyDepthResources() {
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
                              gpuMaterialBuffers,
                              sizeof(UniformBufferObject), assetManager);
        }

        void createForwardPass() {
            forwardPass.create(device, HdrBuffer::Format, depthBuffer.format(),
                               msaa.sampleCount(), shadowPass.descriptorSetLayout(), assetManager);
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
                           sizeof(UniformBufferObject), assetManager);
        }

        void createTonemapPass() {
            tonemapPass.create(device, swapchain.format(), swapchain.extent(),
                               swapchain.imageViews(), hdrBuffer.imageView(),
                               hdrBuffer.sampler(), assetManager);
        }

        void createUIResources() {
            const VkExtent2D extent = swapchain.extent();
            canvas.resize(extent.width, extent.height);

            if (canvas.empty()) {
                auto panel = std::make_unique<UI::PanelElement>(
                    Math::Color{0.025f, 0.035f, 0.055f, 0.82f});
                panel->rectTransform.anchorMin = {0.0f, 0.0f};
                panel->rectTransform.anchorMax = {0.0f, 0.0f};
                panel->rectTransform.offsetMin = {20.0f, 20.0f};
                panel->rectTransform.offsetMax = {300.0f, 110.0f};

                auto accent = std::make_unique<UI::PanelElement>(
                    Math::Color{0.10f, 0.75f, 0.90f, 1.0f});
                accent->rectTransform.anchorMin = {0.0f, 0.0f};
                accent->rectTransform.anchorMax = {0.0f, 1.0f};
                accent->rectTransform.offsetMin = {0.0f, 0.0f};
                accent->rectTransform.offsetMax = {4.0f, 0.0f};
                panel->addChild(std::move(accent));

                static_cast<void>(canvas.addElement(std::move(panel)));
            }

            canvasRenderer.create(
                vulkanDevice.physical(), device, swapchain.format(), extent,
                swapchain.imageViews(), MAX_FRAMES_IN_FLIGHT, assetManager);
        }

        void createMeshBuffers() {
            Mesh sceneMesh;
            renderables.clear();
            glm::vec3 sceneMinimum{std::numeric_limits<float>::max()};
            glm::vec3 sceneMaximum{std::numeric_limits<float>::lowest()};
            // Each MeshRenderer retains its own draw range, but identical
            // meshes contribute their geometry to the GPU buffers only once.
            std::unordered_map<const Mesh*, uint32_t> firstIndices;
            registry.view<Transform, MeshRenderer>(
                [&](const Entity entity, const Transform& transform, MeshRenderer& renderer) {
                    if (!renderer.hasMesh()) {
                        return;
                    }

                    const Mesh* const mesh = renderer.mesh.get();
                    if (const auto existing = firstIndices.find(mesh);
                        existing != firstIndices.end()) {
                        renderer.firstIndex = existing->second;
                    } else {
                        if (sceneMesh.vertices.size() + mesh->vertices.size() >
                                std::numeric_limits<uint32_t>::max() ||
                            sceneMesh.indices.size() + mesh->indices.size() >
                                std::numeric_limits<uint32_t>::max()) {
                            throw std::runtime_error("Scene geometry exceeds 32-bit draw limits");
                        }
                        const uint32_t vertexOffset = sceneMesh.vertexCount();
                        renderer.firstIndex = sceneMesh.indexCount();
                        firstIndices.emplace(mesh, renderer.firstIndex);
                        sceneMesh.vertices.insert(sceneMesh.vertices.end(),
                                                  mesh->vertices.begin(), mesh->vertices.end());
                        for (const uint32_t index : mesh->indices) {
                            sceneMesh.indices.push_back(vertexOffset + index);
                        }
                    }

                    AABB localBounds{
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
                    renderables.push_back({entity, localBounds});

                    const AABB worldBounds =
                        localBounds.transformed(transform.matrix().native());
                    sceneMinimum = glm::min(sceneMinimum, worldBounds.min.native());
                    sceneMaximum = glm::max(sceneMaximum, worldBounds.max.native());
                });

            if (sceneMesh.empty()) {
                throw std::runtime_error("Scene contains no renderable geometry");
            }

            const glm::vec3 center = (sceneMinimum + sceneMaximum) * 0.5f;
            const glm::vec3 halfExtent = (sceneMaximum - sceneMinimum) * 0.5f;
            sceneCenter = Vec3{center};
            sceneRadius = std::max({halfExtent.x, halfExtent.y, halfExtent.z, 1.0f});

            vertexBuffer.createDeviceLocal(vulkanDevice.physical(), device, sceneMesh.vertices.data(),
                sizeof(Vertex) * sceneMesh.vertices.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                commandPool, vulkanDevice.graphicsQueue());
            indexBuffer.createDeviceLocal(vulkanDevice.physical(), device, sceneMesh.indices.data(),
                sizeof(uint32_t) * sceneMesh.indices.size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                commandPool, vulkanDevice.graphicsQueue());
        }

        void createInstanceBuffer() {
            instanceModels.resize(renderables.size());
            shadowInstanceModels.resize(renderables.size());
            materials.resize(renderables.size());
            updateRenderableBuffers();
            for (Buffer& buffer : instanceBuffers) {
                buffer.createHostVisible(vulkanDevice.physical(), device,
                    sizeof(glm::mat4) * instanceModels.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
                buffer.update(instanceModels.data(), sizeof(glm::mat4) * instanceModels.size());
            }
            for (Buffer& buffer : shadowInstanceBuffers) {
                buffer.createHostVisible(vulkanDevice.physical(), device,
                    sizeof(glm::mat4) * shadowInstanceModels.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
                buffer.update(shadowInstanceModels.data(), sizeof(glm::mat4) * shadowInstanceModels.size());
            }
            for (Buffer& buffer : materialBuffers) {
                buffer.createHostVisible(vulkanDevice.physical(), device,
                    sizeof(GPUMaterialData) * materials.size(),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
                buffer.update(materials.data(), sizeof(GPUMaterialData) * materials.size());
            }
        }

        void updateRenderableBuffers() {
            for (std::size_t index = 0; index < renderables.size(); ++index) {
                const Entity entity = renderables[index].entity;
                const Transform& transform = registry.get<Transform>(entity);
                const MeshRenderer& renderer = registry.get<MeshRenderer>(entity);
                const glm::mat4 model = transform.matrix().native();
                shadowInstanceModels[index] = model;
                instanceModels[index] = model;
                materials[index] = {
                    glm::vec4{renderer.material.baseColor.r(),
                              renderer.material.baseColor.g(),
                              renderer.material.baseColor.b(),
                              renderer.material.metallic},
                    glm::vec4{renderer.material.roughness,
                              renderer.material.ambientOcclusion, 0.0f, 0.0f},
                };
                if (gpuObjects.size() == renderables.size()) {
                    std::memcpy(gpuObjects[index].model.data, &model, sizeof(model));
                    gpuObjects[index].castShadow = renderer.castShadow ? 1u : 0u;
                }
            }
            if (instanceBuffers[currentFrame].handle() != VK_NULL_HANDLE) {
                instanceBuffers[currentFrame].update(instanceModels.data(),
                                                     sizeof(glm::mat4) * instanceModels.size());
                shadowInstanceBuffers[currentFrame].update(shadowInstanceModels.data(),
                                                           sizeof(glm::mat4) * shadowInstanceModels.size());
                materialBuffers[currentFrame].update(
                    materials.data(), sizeof(GPUMaterialData) * materials.size());
                if (cullingObjectBuffers[currentFrame].handle() != VK_NULL_HANDLE) {
                    cullingObjectBuffers[currentFrame].update(
                        gpuObjects.data(),
                        sizeof(Culling::GPUObjectData) * gpuObjects.size());
                }
            }
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
            data.enableOcclusionCulling = 1;
            data.viewportWidth = static_cast<float>(swapchain.extent().width);
            data.viewportHeight = static_cast<float>(swapchain.extent().height);
            data.depthBias = 0.0025f;
            data.aabbExpansion = 0.01f;
            // Never reject objects using an uninitialized hierarchy.
            data.cameraCut = hiZValid ? 0u : 1u;
            data.shadowPass = 0;
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
            shadowCullingUniformBuffers[frame].update(&data, sizeof(data));
        }

        void createUniformBuffers() {
            for (Buffer& buffer : uniformBuffers) {
                buffer.createHostVisible(vulkanDevice.physical(), device, sizeof(UniformBufferObject),
                                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
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
            const auto objectCount = static_cast<uint32_t>(renderables.size());
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
                const Entity entity = renderables[i].entity;
                const glm::mat4 model =
                    registry.get<Transform>(entity).matrix().native();
                const MeshRenderer& renderer = registry.get<MeshRenderer>(entity);
                auto& object = gpuObjects[i];
                std::memcpy(object.model.data, &model, sizeof(model));
                const AABB& bounds = renderables[i].localBounds;
                object.localAabbMin = {
                    bounds.min.x(), bounds.min.y(), bounds.min.z(), 0.0f};
                object.localAabbMax = {
                    bounds.max.x(), bounds.max.y(), bounds.max.z(), 0.0f};
                object.indexCount = renderer.mesh->indexCount();
                object.instanceCount = 1;
                object.firstIndex = renderer.firstIndex;
                object.vertexOffset = 0;
                object.firstInstance = i;
                object.castShadow = renderer.castShadow ? 1u : 0u;
            }
            for (Buffer& buffer : cullingObjectBuffers) {
                buffer.createHostVisible(
                    vulkanDevice.physical(), device,
                    sizeof(Culling::GPUObjectData) * gpuObjects.size(),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
                buffer.update(gpuObjects.data(),
                              sizeof(Culling::GPUObjectData) * gpuObjects.size());
            }

            for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
                cullingUniformBuffers[frame].createHostVisible(vulkanDevice.physical(), device,
                    sizeof(Culling::CullingUniformData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
                shadowCullingUniformBuffers[frame].createHostVisible(vulkanDevice.physical(), device,
                    sizeof(Culling::CullingUniformData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
                std::vector<VkDrawIndexedIndirectCommand> emptyCommands(objectCount);
                indirectBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device, emptyCommands.data(),
                    sizeof(VkDrawIndexedIndirectCommand) * objectCount,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue());
                shadowIndirectBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device, emptyCommands.data(),
                    sizeof(VkDrawIndexedIndirectCommand) * objectCount,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue());
                constexpr uint32_t zero = 0;
                drawCountBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device, &zero, sizeof(zero),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue());
                shadowDrawCountBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device, &zero, sizeof(zero),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue());
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
                hiZReduceDescriptorSetLayout, hiZBuffer, depthBuffer.imageView(), depthBuffer.sampler());

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
            const float extent = sceneRadius;
            const Vec3 lightTarget = sceneCenter;
            const Vec3 lightPosition = lightTarget + Vec3{
                extent * 2.6f, extent * 3.5f, extent * 2.6f};
            constexpr Vec3 worldUp{0.0f, 1.0f, 0.0f};
            const Mat4 lightView = Mat4::lookAt(lightPosition, lightTarget, worldUp);
            const Mat4 lightProjection = Mat4::scale(
                Mat4::ortho(-extent * 2.25f, extent * 2.25f,
                            -extent * 2.25f, extent * 2.25f,
                            0.1f, extent * 8.0f),
                Vec3{1.0f, -1.0f, 1.0f});
            return lightProjection * lightView;
        }

        void updateUniformBuffer(const uint32_t frame) {
            Entity activeCamera = NullEntity;
            registry.view<CameraComponent, Transform>(
                [&](const Entity entity, CameraComponent& component, Transform&) {
                    if (activeCamera == NullEntity && component.primary) {
                        activeCamera = entity;
                    }
                });
            if (activeCamera == NullEntity) {
                throw std::runtime_error("Scene has no primary CameraComponent with Transform");
            }

            const CameraComponent& component = registry.get<CameraComponent>(activeCamera);
            const Transform& transform = registry.get<Transform>(activeCamera);
            if (!component.isPerspective() || !component.isValid()) {
                throw std::runtime_error("Primary CameraComponent has unsupported settings");
            }

            camera.emplace(Degrees{component.fieldOfView}, component.aspectRatio,
                           component.nearClip, component.farClip);
            camera->setPosition(transform.position);
            camera->setRotation(Degrees{transform.rotation.y()},
                                Degrees{transform.rotation.x()});

            const UniformBufferObject data{
                camera->viewMatrix(), camera->projectionMatrix(), lightSpaceMatrix(),
                glm::vec4{camera->position().native(), 1.0f},
                glm::vec4{-0.45f, -0.80f, -0.35f, 4.0f}};
            uniformBuffers[frame].update(&data, sizeof(data));
        }

        void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
            VkCommandBufferBeginInfo beginInfo{};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

            if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
                throw std::runtime_error("Could not begin command buffer");
            }
            shadowPass.record(
                commandBuffer, lightSpaceMatrix(), vertexBuffer.handle(),
                shadowInstanceBuffers[currentFrame].handle(), indexBuffer.handle(),
                shadowCullingPasses[currentFrame],
                shadowIndirectDraws[currentFrame],
                static_cast<std::uint32_t>(gpuObjects.size()));

            gpuCullingPasses[currentFrame].record(
                commandBuffer, static_cast<std::uint32_t>(gpuObjects.size()));

            forwardPass.begin(
                commandBuffer, hdrFramebuffer, swapchain.extent(),
                shadowPass.descriptorSet(currentFrame), vertexBuffer.handle(),
                instanceBuffers[currentFrame].handle(), indexBuffer.handle());
            forwardPass.draw(commandBuffer, indirectDraws[currentFrame]);
            skyPass.record(commandBuffer, currentFrame);
            forwardPass.end(commandBuffer);

            VkImageMemoryBarrier2 depthReady{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            depthReady.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            depthReady.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            depthReady.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            depthReady.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            depthReady.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            depthReady.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            depthReady.image = depthBuffer.image();
            depthReady.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            VkDependencyInfo depthDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            depthDependency.imageMemoryBarrierCount = 1;
            depthDependency.pImageMemoryBarriers = &depthReady;
            vkCmdPipelineBarrier2(commandBuffer, &depthDependency);
            hiZPass.record(commandBuffer, hiZBuffer, hiZValid);
            hiZValid = true;

            tonemapPass.record(commandBuffer, imageIndex, swapchain.extent());
            canvasRenderer.record(canvas, commandBuffer, imageIndex, currentFrame,
                                  swapchain.extent());

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
            canvasRenderer.destroy();
            tonemapPass.destroy();
            if (hdrFramebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device, hdrFramebuffer, nullptr);
                hdrFramebuffer = VK_NULL_HANDLE;
            }

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
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            while (w == 0 || h == 0) {
                SDL_Event e;
                SDL_WaitEvent(&e);
                SDL_GetWindowSizeInPixels(window, &w, &h);
            }

            vkDeviceWaitIdle(device);

            destroyCullingResources();

            canvasRenderer.destroy();
            tonemapPass.destroy();
            if (hdrFramebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device, hdrFramebuffer, nullptr);
                hdrFramebuffer = VK_NULL_HANDLE;
            }
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
            createTonemapPass();
            createUIResources();
            createCullingResources();
        }

        // ---------- MAIN LOOP ----------

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
            updateRenderableBuffers();
            updateCullingUniformBuffer(currentFrame);
            updateShadowCullingUniformBuffer(currentFrame);
            recordCommandBuffer(commandBuffers[currentFrame], imageIndex);

            VkSubmitInfo submitInfo{};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

            VkSemaphore waitSemaphores[] = {imageAvailableSemaphores[currentFrame]};
            VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
            submitInfo.waitSemaphoreCount = 1;
            submitInfo.pWaitSemaphores = waitSemaphores;
            submitInfo.pWaitDstStageMask = waitStages;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &commandBuffers[currentFrame];

            VkSemaphore signalSemaphores[] = {renderFinishedSemaphores[imageIndex]};
            submitInfo.signalSemaphoreCount = 1;
            submitInfo.pSignalSemaphores = signalSemaphores;

            if (vkQueueSubmit(vulkanDevice.graphicsQueue(), 1, &submitInfo,
                              inFlightFences[currentFrame]) != VK_SUCCESS) {
                throw std::runtime_error("Could not submit command buffer to queue");
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
            }
        }

        void mainLoop() {
            bool running = true;
            SDL_Event event;
            Time::init();
            while (running) {
                Input::beginFrame();
                while (SDL_PollEvent(&event)) {
                    SDLInput::processEvent(event);
                    if (event.type == SDL_EVENT_QUIT) {
                        running = false;
                    } else if (event.type == SDL_EVENT_WINDOW_RESIZED ||
                               event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                        framebufferResized = true;
                               } else if (event.type == SDL_EVENT_KEY_DOWN) {
                                   if (event.key.key == SDLK_ESCAPE) running = false;
                               }
                }

                if (!running) {
                    break;
                }

                Time::update();
                drawFrame();
                updateFpsCounter();
            }
            vkDeviceWaitIdle(device);
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
                forwardPass.destroy();
                shadowPass.destroy();
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

            if (window != nullptr) {
                SDL_DestroyWindow(window);
                window = nullptr;
            }
            SDL_Quit();
        }
    };

} // namespace

void Renderer::run(Registry& registry) {
    RenderApp app{registry};
    app.run();
}

} // namespace Engine
