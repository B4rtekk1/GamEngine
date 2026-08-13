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
#include "Engine/Renderer/Vulkan/shadow_map.h"
#include "Engine/Renderer/shader_loader.h"
#include "Engine/Renderer/Vulkan/graphics_pipeline.h"
#include "Engine/Renderer/Vulkan/buffer.h"
#include "Engine/Renderer/Vulkan/vulkan_device.h"
#include "Engine/Renderer/Vulkan/swapchain.h"
#include "Engine/Renderer/Geometry/Vertex.h"
#include "Engine/Renderer/Geometry/Mesh.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Core/Camera.h"
#include "Engine/Math/AABB.h"
#include "Engine/Math/Frustum.h"
#include "Engine/Math/Math.h"
#include "Engine/Core/Time.h"
#include "Engine/Renderer/Skybox/Skybox.h"
#include "Engine/Input/Input.h"
#include "Platform/SDL/SDLInput.h"

#include <cstdint>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace Engine {

constexpr uint32_t WIDTH = 800;
constexpr uint32_t HEIGHT = 600;
constexpr int MAX_FRAMES_IN_FLIGHT = 2;

namespace {
    constexpr float SHADOW_DEPTH_BIAS_CONSTANT = 0.15f;
    constexpr float SHADOW_DEPTH_BIAS_SLOPE = 0.35f;

    struct PushConstants {
        glm::vec4 baseColorMetallic;
        glm::vec4 roughnessAo;
    };

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
    class CubeApp {
    public:
        ~CubeApp() {
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
        std::vector<VkFramebuffer> swapChainFramebuffers;

        MsaaResources msaa;

        GraphicsPipeline graphicsPipeline;
        Skybox skybox;
        DepthBuffer depthBuffer;
        ShadowMap shadowMap;
        VkDescriptorSetLayout shadowDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout skyboxDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool shadowDescriptorPool = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> descriptorSets;
        VkPipelineLayout shadowPipelineLayout = VK_NULL_HANDLE;
        VkPipeline shadowPipeline = VK_NULL_HANDLE;
        Scene scene;
        Camera camera{Degrees{45.0f}, static_cast<float>(WIDTH) / static_cast<float>(HEIGHT),
                      0.1f, Scene::GridHalfExtent * 10.0f};
        Buffer vertexBuffer;
        Buffer indexBuffer;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> instanceBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> shadowInstanceBuffers;
        std::array<Buffer, MAX_FRAMES_IN_FLIGHT> uniformBuffers;
        std::vector<glm::mat4> instanceModels;
        std::vector<glm::mat4> shadowInstanceModels;
        uint32_t visibleCubeCount = 0;

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

        // ---------- INIT ----------

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
            createInstance();
            setupDebugMessenger();
            createSurface();
            vulkanDevice.create(instance, surface);
            device = vulkanDevice.logical();
            depthBuffer.initialize(vulkanDevice.physical(), device);
            msaa.initialize(vulkanDevice.physical(), device, VK_SAMPLE_COUNT_4_BIT);
            createSwapChain();
            msaa.create(swapchain.extent(), swapchain.format());
            createDepthResources();
            createCommandPool();
            createMeshBuffers();
            createInstanceBuffer();
            createUniformBuffers();
            createShadowResources();
            createGraphicsPipeline();
            createSkyboxResources();
            createFramebuffers();
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
            appInfo.apiVersion = VK_API_VERSION_1_2;

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
            auto func = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (func != nullptr) func(instance, debugMessenger, pAllocator);
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

        // ---------- SWAPCHAIN ----------

        void createSwapChain() {
            swapchain.create(window, surface, vulkanDevice);
        }

    // ---------- RENDER PASS / PIPELINE ----------

    void createDepthResources() {
        depthBuffer.create(swapchain.extent(), msaa.sampleCount());
    }

    void destroyDepthResources() {
        depthBuffer.destroy();
    }

#if 0 // Superseded by GraphicsPipeline.
    void createRenderPass() {
            VkAttachmentDescription colorAttachment{};
            colorAttachment.format = swapchain.format();
            colorAttachment.samples = msaa.sampleCount();
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAttachment.storeOp = msaa.enabled()
                ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                : VK_ATTACHMENT_STORE_OP_STORE;
            colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            colorAttachment.finalLayout = msaa.enabled()
                ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

            VkAttachmentDescription resolveAttachment{};
            resolveAttachment.format = swapchain.format();
            resolveAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
            resolveAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            resolveAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            resolveAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            resolveAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            resolveAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        resolveAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentDescription depthAttachment{};
        depthAttachment.format = depthBuffer.format();
        depthAttachment.samples = msaa.sampleCount();
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

            VkAttachmentReference colorAttachmentRef{};
            colorAttachmentRef.attachment = 0;
            colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference resolveAttachmentRef{};
        resolveAttachmentRef.attachment = 2;
        resolveAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference depthAttachmentRef{};
        depthAttachmentRef.attachment = 1;
        depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorAttachmentRef;
        subpass.pResolveAttachments = msaa.enabled() ? &resolveAttachmentRef : nullptr;
        subpass.pDepthStencilAttachment = &depthAttachmentRef;

            VkSubpassDependency dependency{};
            dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
            dependency.dstSubpass = 0;
            dependency.srcStageMask =
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
            dependency.srcAccessMask = 0;
            dependency.dstStageMask =
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
            dependency.dstAccessMask =
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

            std::vector<VkAttachmentDescription> attachments = {colorAttachment, depthAttachment};
            if (msaa.enabled()) {
                attachments.push_back(resolveAttachment);
            }

            VkRenderPassCreateInfo renderPassInfo{};
            renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
            renderPassInfo.pAttachments = attachments.data();
            renderPassInfo.subpassCount = 1;
            renderPassInfo.pSubpasses = &subpass;
            renderPassInfo.dependencyCount = 1;
            renderPassInfo.pDependencies = &dependency;

            if (vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS) {
                throw std::runtime_error("Could not create render pass");
            }
        }

        void createGraphicsPipeline() {
            const auto vertShaderModule = vkutil::loadShaderModule(device, "shaders/vert.spv");
            const auto fragShaderModule = vkutil::loadShaderModule(device, "shaders/frag.spv");

            VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
            vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
            vertShaderStageInfo.module = vertShaderModule.get();
            vertShaderStageInfo.pName = "main";

            VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
            fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            fragShaderStageInfo.module = fragShaderModule.get();
            fragShaderStageInfo.pName = "main";

            VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};

            VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
            vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            vertexInputInfo.vertexBindingDescriptionCount = 0;
            vertexInputInfo.vertexAttributeDescriptionCount = 0;

            VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
            inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            inputAssembly.primitiveRestartEnable = VK_FALSE;

            VkPipelineViewportStateCreateInfo viewportState{};
            viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            viewportState.viewportCount = 1;
            viewportState.scissorCount = 1;

            VkPipelineRasterizationStateCreateInfo rasterizer{};
            rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rasterizer.depthClampEnable = VK_FALSE;
            rasterizer.rasterizerDiscardEnable = VK_FALSE;
            rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
            rasterizer.lineWidth = 1.0f;
            rasterizer.cullMode = VK_CULL_MODE_NONE;
            rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
            rasterizer.depthBiasEnable = VK_FALSE;

            VkPipelineMultisampleStateCreateInfo multisampling{};
            multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            multisampling.sampleShadingEnable = VK_FALSE;
            multisampling.rasterizationSamples = msaa.sampleCount();

            VkPipelineDepthStencilStateCreateInfo depthStencil{};
            depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            depthStencil.depthTestEnable = VK_TRUE;
            depthStencil.depthWriteEnable = VK_TRUE;
            depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

            VkPipelineColorBlendAttachmentState colorBlendAttachment{};
            colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            colorBlendAttachment.blendEnable = VK_FALSE;

            VkPipelineColorBlendStateCreateInfo colorBlending{};
            colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            colorBlending.logicOpEnable = VK_FALSE;
            colorBlending.attachmentCount = 1;
            colorBlending.pAttachments = &colorBlendAttachment;

            std::vector<VkDynamicState> dynamicStates = {
                VK_DYNAMIC_STATE_VIEWPORT,
                VK_DYNAMIC_STATE_SCISSOR
            };
            VkPipelineDynamicStateCreateInfo dynamicState{};
            dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
            dynamicState.pDynamicStates = dynamicStates.data();

            VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
            pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            pipelineLayoutInfo.setLayoutCount = 0;
            VkPushConstantRange pushConstantRange{};
            pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            pushConstantRange.offset = 0;
            pushConstantRange.size = sizeof(PushConstants);
            pipelineLayoutInfo.pushConstantRangeCount = 1;
            pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

            if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
                throw std::runtime_error("Could not create pipeline layout");
            }

            VkGraphicsPipelineCreateInfo pipelineInfo{};
            pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            pipelineInfo.stageCount = 2;
            pipelineInfo.pStages = shaderStages;
            pipelineInfo.pVertexInputState = &vertexInputInfo;
            pipelineInfo.pInputAssemblyState = &inputAssembly;
            pipelineInfo.pViewportState = &viewportState;
            pipelineInfo.pRasterizationState = &rasterizer;
            pipelineInfo.pMultisampleState = &multisampling;
            pipelineInfo.pDepthStencilState = &depthStencil;
            pipelineInfo.pColorBlendState = &colorBlending;
            pipelineInfo.pDynamicState = &dynamicState;
            pipelineInfo.layout = pipelineLayout;
            pipelineInfo.renderPass = renderPass;
            pipelineInfo.subpass = 0;
            pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

            if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline) != VK_SUCCESS) {
                throw std::runtime_error("Could not create graphics pipeline");
            }

        }

#endif

        void createGraphicsPipeline() {
            GraphicsPipelineOptions options{};
            options.colorFormat = swapchain.format();
            options.depthFormat = depthBuffer.format();
            options.samples = msaa.sampleCount();
            options.vertexShader = "shaders/pbr.vert.spv";
            options.fragmentShader = "shaders/pbr.frag.spv";
            options.pushConstantSize = sizeof(PushConstants);
            options.pushConstantStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            options.cullMode = VK_CULL_MODE_BACK_BIT;
            options.descriptorSetLayouts = {shadowDescriptorSetLayout};
            options.vertexBindings = {
                {.binding = 0, .stride = sizeof(Vertex), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX},
                {.binding = 1, .stride = sizeof(glm::mat4), .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE},
            };
            options.vertexAttributes = {
                {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, position)},
                {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, color)},
                {.location = 3, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, normal)},
                {.location = 4, .binding = 1, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0},
                {.location = 5, .binding = 1, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = sizeof(glm::vec4)},
                {.location = 6, .binding = 1, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = sizeof(glm::vec4) * 2},
                {.location = 7, .binding = 1, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = sizeof(glm::vec4) * 3},
            };
            graphicsPipeline.create(device, options);
        }

        void createShadowResources() {
            shadowMap.create(vulkanDevice.physical(), device);

            VkDescriptorSetLayoutBinding bindings[2]{};
            bindings[0].binding = 0;
            bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            bindings[1].binding = 1;
            bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            bindings[1].descriptorCount = 1;
            bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            layoutInfo.bindingCount = 2;
            layoutInfo.pBindings = bindings;
            if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &shadowDescriptorSetLayout) != VK_SUCCESS) {
                throw std::runtime_error("Could not create shadow descriptor-set layout");
            }
            VkDescriptorPoolSize poolSizes[2] = {
                {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_FRAMES_IN_FLIGHT},
                {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_FRAMES_IN_FLIGHT},
            };
            VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            poolInfo.maxSets = MAX_FRAMES_IN_FLIGHT;
            poolInfo.poolSizeCount = 2;
            poolInfo.pPoolSizes = poolSizes;
            if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &shadowDescriptorPool) != VK_SUCCESS) {
                throw std::runtime_error("Could not create shadow descriptor pool");
            }
            VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            allocateInfo.descriptorPool = shadowDescriptorPool;
            std::vector<VkDescriptorSetLayout> layouts(MAX_FRAMES_IN_FLIGHT, shadowDescriptorSetLayout);
            descriptorSets.resize(MAX_FRAMES_IN_FLIGHT);
            allocateInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
            allocateInfo.pSetLayouts = layouts.data();
            if (vkAllocateDescriptorSets(device, &allocateInfo, descriptorSets.data()) != VK_SUCCESS) {
                throw std::runtime_error("Could not allocate shadow descriptor set");
            }
            VkDescriptorImageInfo imageInfo{};
            imageInfo.sampler = shadowMap.sampler();
            imageInfo.imageView = shadowMap.imageView();
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
                VkDescriptorBufferInfo bufferInfo{};
                bufferInfo.buffer = uniformBuffers[frame].handle();
                bufferInfo.range = sizeof(UniformBufferObject);
                VkWriteDescriptorSet writes[2]{};
                writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[0].dstSet = descriptorSets[frame];
                writes[0].dstBinding = 0;
                writes[0].descriptorCount = 1;
                writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[0].pImageInfo = &imageInfo;
                writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[1].dstSet = descriptorSets[frame];
                writes[1].dstBinding = 1;
                writes[1].descriptorCount = 1;
                writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                writes[1].pBufferInfo = &bufferInfo;
                vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
            }

            const auto shader = vkutil::loadShaderModule(device, "shaders/shadow.vert.spv");
            VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
            stage.module = shader.get();
            stage.pName = "main";
            VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4)};
            VkPipelineLayoutCreateInfo shadowLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            shadowLayoutInfo.pushConstantRangeCount = 1;
            shadowLayoutInfo.pPushConstantRanges = &range;
            if (vkCreatePipelineLayout(device, &shadowLayoutInfo, nullptr, &shadowPipelineLayout) != VK_SUCCESS) {
                throw std::runtime_error("Could not create shadow pipeline layout");
            }
            const VkVertexInputBindingDescription vertexBindings[] = {
                {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX},
                {1, sizeof(glm::mat4), VK_VERTEX_INPUT_RATE_INSTANCE},
            };
            const VkVertexInputAttributeDescription attributes[] = {
                {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
                {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
                {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(glm::vec4)},
                {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(glm::vec4) * 2},
                {7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(glm::vec4) * 3},
            };
            VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            vertexInput.vertexBindingDescriptionCount = std::size(vertexBindings);
            vertexInput.pVertexBindingDescriptions = vertexBindings;
            vertexInput.vertexAttributeDescriptionCount = std::size(attributes);
            vertexInput.pVertexAttributeDescriptions = attributes;
            VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            viewport.viewportCount = 1; viewport.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo rasterizer{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
            rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
            // Use the same winding as the colour pass.  Rendering the opposite
            // faces into the shadow map made cube surfaces shadow themselves.
            rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
            rasterizer.lineWidth = 1.0f;
            rasterizer.depthBiasEnable = VK_TRUE;
            VkPipelineMultisampleStateCreateInfo multisampling{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            depth.depthTestEnable = VK_TRUE; depth.depthWriteEnable = VK_TRUE; depth.depthCompareOp = VK_COMPARE_OP_LESS;
            const std::vector dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS};
            VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            dynamic.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
            dynamic.pDynamicStates = dynamicStates.data();
            VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            pipelineInfo.stageCount = 1; pipelineInfo.pStages = &stage;
            pipelineInfo.pVertexInputState = &vertexInput; pipelineInfo.pInputAssemblyState = &assembly;
            pipelineInfo.pViewportState = &viewport; pipelineInfo.pRasterizationState = &rasterizer;
            pipelineInfo.pMultisampleState = &multisampling; pipelineInfo.pDepthStencilState = &depth;
            pipelineInfo.pDynamicState = &dynamic; pipelineInfo.layout = shadowPipelineLayout;
            pipelineInfo.renderPass = shadowMap.renderPass();
            if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &shadowPipeline) != VK_SUCCESS) {
                throw std::runtime_error("Could not create shadow pipeline");
            }
        }

        void createSkyboxResources() {
            VkDescriptorSetLayoutBinding bindings[2]{};
            bindings[0].binding = 0;
            bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            bindings[1].binding = 1;
            bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[1].descriptorCount = 1;
            bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            layoutInfo.bindingCount = 2;
            layoutInfo.pBindings = bindings;
            if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &skyboxDescriptorSetLayout) != VK_SUCCESS) {
                throw std::runtime_error("Could not create skybox descriptor-set layout");
            }
            std::vector<VkBuffer> buffers;
            buffers.reserve(uniformBuffers.size());
            for (const Buffer& buffer : uniformBuffers) buffers.push_back(buffer.handle());
            skybox.create(vulkanDevice.physical(), device, commandPool, vulkanDevice.graphicsQueue(),
                          graphicsPipeline.renderPass(), swapchain.format(), msaa.sampleCount(),
                          skyboxDescriptorSetLayout, buffers, sizeof(UniformBufferObject));
        }

        void destroySkyboxResources() noexcept {
            skybox.destroy();
            if (skyboxDescriptorSetLayout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(device, skyboxDescriptorSetLayout, nullptr);
                skyboxDescriptorSetLayout = VK_NULL_HANDLE;
            }
        }

        void destroyShadowResources() noexcept {
            if (shadowPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, shadowPipeline, nullptr);
            if (shadowPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, shadowPipelineLayout, nullptr);
            if (shadowDescriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, shadowDescriptorPool, nullptr);
            if (shadowDescriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, shadowDescriptorSetLayout, nullptr);
            shadowPipeline = VK_NULL_HANDLE; shadowPipelineLayout = VK_NULL_HANDLE;
            shadowDescriptorPool = VK_NULL_HANDLE; shadowDescriptorSetLayout = VK_NULL_HANDLE; descriptorSets.clear();
            shadowMap.destroy();
        }

        void createMeshBuffers() {
            Mesh sceneMesh;
            // Each MeshRenderer retains its own draw range, but identical
            // meshes contribute their geometry to the GPU buffers only once.
            std::unordered_map<const Mesh*, uint32_t> firstIndices;
            scene.registry.view<Transform, MeshRenderer>(
                [&](Entity, Transform&, MeshRenderer& renderer) {
                    if (!renderer.hasMesh()) {
                        return;
                    }

                    const Mesh* const mesh = renderer.mesh.get();
                    if (const auto existing = firstIndices.find(mesh); existing != firstIndices.end()) {
                        renderer.firstIndex = existing->second;
                        return;
                    }

                    const uint32_t vertexOffset = sceneMesh.vertexCount();
                    renderer.firstIndex = sceneMesh.indexCount();
                    firstIndices.emplace(mesh, renderer.firstIndex);
                    sceneMesh.vertices.insert(sceneMesh.vertices.end(),
                                              mesh->vertices.begin(), mesh->vertices.end());
                    for (const uint32_t index : mesh->indices) {
                        sceneMesh.indices.push_back(vertexOffset + index);
                    }
                });

            if (sceneMesh.empty()) {
                throw std::runtime_error("Scene contains no renderable geometry");
            }

            vertexBuffer.createDeviceLocal(vulkanDevice.physical(), device, sceneMesh.vertices.data(),
                sizeof(Vertex) * sceneMesh.vertices.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                commandPool, vulkanDevice.graphicsQueue());
            indexBuffer.createDeviceLocal(vulkanDevice.physical(), device, sceneMesh.indices.data(),
                sizeof(uint32_t) * sceneMesh.indices.size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                commandPool, vulkanDevice.graphicsQueue());
        }

        void createInstanceBuffer() {
            instanceModels.resize(Scene::CubeCount + 1);
            shadowInstanceModels.resize(Scene::CubeCount + 1);
            updateInstanceBuffer();
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
        }

        void updateInstanceBuffer() {
            const glm::mat4 planeModel = scene.registry.get<Transform>(scene.plane).matrix().native();
            instanceModels[0] = planeModel;
            shadowInstanceModels[0] = planeModel;
            const Frustum frustum{camera.projectionMatrix().native() * camera.viewMatrix().native()};
            visibleCubeCount = 0;
            for (std::size_t index = 0; index < scene.cubes.size(); ++index) {
                const Transform& transform = scene.registry.get<Transform>(scene.cubes[index]);
                const glm::mat4 model = transform.matrix().native();
                shadowInstanceModels[index + 1] = model;
                if (frustum.intersects(AABB::unitCube().transformed(model))) {
                    instanceModels[++visibleCubeCount] = model;
                }
            }
            if (instanceBuffers[currentFrame].handle() != VK_NULL_HANDLE) {
                instanceBuffers[currentFrame].update(instanceModels.data(),
                                                     sizeof(glm::mat4) * (visibleCubeCount + 1));
                shadowInstanceBuffers[currentFrame].update(shadowInstanceModels.data(),
                                                           sizeof(glm::mat4) * shadowInstanceModels.size());
            }
        }

        void createUniformBuffers() {
            for (Buffer& buffer : uniformBuffers) {
                buffer.createHostVisible(vulkanDevice.physical(), device, sizeof(UniformBufferObject),
                                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
            }
        }

        void createFramebuffers() {
            const auto& imageViews = swapchain.imageViews();
            const VkExtent2D extent = swapchain.extent();
            swapChainFramebuffers.resize(imageViews.size());
            for (size_t i = 0; i < imageViews.size(); i++) {
                VkImageView msaaAttachments[] = {
                    msaa.colorImageView(),
                    depthBuffer.imageView(),
                    imageViews[i]
                };
                VkImageView directAttachment[] = {imageViews[i], depthBuffer.imageView()};

                VkFramebufferCreateInfo framebufferInfo{};
                framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
                framebufferInfo.renderPass = graphicsPipeline.renderPass();
                framebufferInfo.attachmentCount = msaa.enabled() ? 3u : 2u;
                framebufferInfo.pAttachments = msaa.enabled()
                    ? msaaAttachments
                    : directAttachment;
                framebufferInfo.width = extent.width;
                framebufferInfo.height = extent.height;
                framebufferInfo.layers = 1;

                if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &swapChainFramebuffers[i]) != VK_SUCCESS) {
                    throw std::runtime_error("Could not create framebuffer");
                }
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

        static Mat4 lightSpaceMatrix() {
            constexpr float extent = Scene::GridHalfExtent;
            const Vec3 lightTarget{0.0f, extent, 0.0f};
            const Vec3 lightPosition = lightTarget + Vec3{extent * 2.6f, extent * 3.5f,
                                                          extent * 2.6f};
            const Vec3 worldUp{0.0f, 1.0f, 0.0f};
            const Mat4 lightView = Mat4::lookAt(lightPosition, lightTarget, worldUp);
            const Mat4 lightProjection = Mat4::scale(
                Mat4::ortho(-extent * 2.25f, extent * 2.25f,
                            -extent * 2.25f, extent * 2.25f,
                            0.1f, extent * 8.0f),
                Vec3{1.0f, -1.0f, 1.0f});
            return lightProjection * lightView;
        }

        void updateUniformBuffer(const uint32_t frame) {
            constexpr float extent = Scene::GridHalfExtent;
            constexpr Vec3 sceneCenter{0.0f, extent, 0.0f};
            constexpr Vec3 cameraOffset{extent * 2.9f, extent * 2.6f, extent * 3.9f};
            const Vec3 centeredCameraPosition = sceneCenter + cameraOffset;
            const Vec3 direction = sceneCenter - centeredCameraPosition;
            const float horizontalDistance = Vec2{direction.x(), direction.z()}.length();
            camera.setPosition(centeredCameraPosition);
            // atan2 returns radians; convert explicitly before storing camera angles.
            camera.setRotation(Degrees{Radians{std::atan2(direction.z(), direction.x())}},
                               Degrees{Radians{std::atan2(direction.y(), horizontalDistance)}});

            const UniformBufferObject data{
                camera.viewMatrix(), camera.projectionMatrix(), lightSpaceMatrix(),
                glm::vec4{centeredCameraPosition.native(), 1.0f},
                glm::vec4{-0.45f, -0.80f, -0.35f, 4.0f}};
            uniformBuffers[frame].update(&data, sizeof(data));
        }

        void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
            VkCommandBufferBeginInfo beginInfo{};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

            if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
                throw std::runtime_error("Could not begin command buffer");
            }

            const Mat4 lightSpace = lightSpaceMatrix();

            VkRenderPassBeginInfo shadowPassInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            shadowPassInfo.renderPass = shadowMap.renderPass();
            shadowPassInfo.framebuffer = shadowMap.framebuffer();
            shadowPassInfo.renderArea.extent = {ShadowMap::Resolution, ShadowMap::Resolution};
            VkClearValue shadowClear{};
            shadowClear.depthStencil = {1.0f, 0};
            shadowPassInfo.clearValueCount = 1;
            shadowPassInfo.pClearValues = &shadowClear;
            vkCmdBeginRenderPass(commandBuffer, &shadowPassInfo, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline);
            const VkBuffer shadowVertexBuffers[] = {vertexBuffer.handle(), shadowInstanceBuffers[currentFrame].handle()};
            constexpr VkDeviceSize shadowVertexOffsets[] = {0, 0};
            vkCmdBindVertexBuffers(commandBuffer, 0, 2, shadowVertexBuffers, shadowVertexOffsets);
            vkCmdBindIndexBuffer(commandBuffer, indexBuffer.handle(), 0, VK_INDEX_TYPE_UINT32);
            VkViewport shadowViewport{0.0f, 0.0f, static_cast<float>(ShadowMap::Resolution), static_cast<float>(ShadowMap::Resolution), 0.0f, 1.0f};
            VkRect2D shadowScissor{{0, 0}, {ShadowMap::Resolution, ShadowMap::Resolution}};
            vkCmdSetViewport(commandBuffer, 0, 1, &shadowViewport);
            vkCmdSetScissor(commandBuffer, 0, 1, &shadowScissor);
            vkCmdSetDepthBias(commandBuffer, SHADOW_DEPTH_BIAS_CONSTANT, 0.0f,
                              SHADOW_DEPTH_BIAS_SLOPE);
            vkCmdPushConstants(commandBuffer, shadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(lightSpace), &lightSpace);
            const auto drawShadowBatch = [&](const MeshRenderer& renderer, const uint32_t instanceCount,
                                             const uint32_t firstInstance) {
                if (!renderer.castShadow || !renderer.hasMesh()) {
                    return;
                }
                vkCmdDrawIndexed(commandBuffer, renderer.mesh->indexCount(), instanceCount,
                                 renderer.firstIndex, 0, firstInstance);
            };
            drawShadowBatch(scene.registry.get<MeshRenderer>(scene.plane), 1, 0);
            drawShadowBatch(scene.registry.get<MeshRenderer>(scene.cubes.front()), Scene::CubeCount, 1);
            vkCmdEndRenderPass(commandBuffer);

            VkRenderPassBeginInfo renderPassInfo{};
            renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            renderPassInfo.renderPass = graphicsPipeline.renderPass();
            renderPassInfo.framebuffer = swapChainFramebuffers[imageIndex];
            renderPassInfo.renderArea.offset = {.x = 0, .y = 0};
            renderPassInfo.renderArea.extent = swapchain.extent();

            VkClearValue clearValues[2]{};
            clearValues[0].color.float32[0] = 0.02f;
            clearValues[0].color.float32[1] = 0.02f;
            clearValues[0].color.float32[2] = 0.05f;
            clearValues[0].color.float32[3] = 1.0f;
            clearValues[1].depthStencil = {.depth = 1.0f, .stencil = 0};
            renderPassInfo.clearValueCount = 2;
            renderPassInfo.pClearValues = clearValues;

            vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline.handle());
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline.layout(), 0, 1,
                                    &descriptorSets[currentFrame], 0, nullptr);
            const VkBuffer vertexBuffers[] = {vertexBuffer.handle(), instanceBuffers[currentFrame].handle()};
            constexpr VkDeviceSize vertexOffsets[] = {0, 0};
            vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, vertexOffsets);
            vkCmdBindIndexBuffer(commandBuffer, indexBuffer.handle(), 0, VK_INDEX_TYPE_UINT32);

            VkViewport viewport{};
            viewport.x = 0.0f;
            viewport.y = 0.0f;
            viewport.width = static_cast<float>(swapchain.extent().width);
            viewport.height = static_cast<float>(swapchain.extent().height);
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

            VkRect2D scissor{};
            scissor.offset = {.x = 0, .y = 0};
            scissor.extent = swapchain.extent();
            vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

            const auto drawBatch = [&](const MeshRenderer& renderer, const uint32_t instanceCount,
                                       const uint32_t firstInstance) {
                if (!renderer.hasMesh()) return;
                const PushConstants constants{
                    glm::vec4{renderer.material.baseColor.native(), renderer.material.metallic},
                    glm::vec4{renderer.material.roughness, renderer.material.ambientOcclusion, 0.0f, 0.0f}};
                vkCmdPushConstants(commandBuffer, graphicsPipeline.layout(),
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(constants), &constants);
                vkCmdDrawIndexed(commandBuffer, renderer.mesh->indexCount(), instanceCount,
                                 renderer.firstIndex, 0, firstInstance);
            };
            drawBatch(scene.registry.get<MeshRenderer>(scene.plane), 1, 0);
            drawBatch(scene.registry.get<MeshRenderer>(scene.cubes.front()), visibleCubeCount, 1);

            skybox.draw(commandBuffer, currentFrame);

            vkCmdEndRenderPass(commandBuffer);

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
                    vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderFinishedSemaphores[i]) != VK_SUCCESS ||
                    vkCreateFence(device, &fenceInfo, nullptr, &inFlightFences[i]) != VK_SUCCESS) {
                    throw std::runtime_error("Could not create synchronization objects");
                    }
            }
        }

        // ---------- SWAPCHAIN RECREATE ----------

        void cleanupSwapChain() {
            for (auto framebuffer : swapChainFramebuffers) {
                vkDestroyFramebuffer(device, framebuffer, nullptr);
            }
            swapChainFramebuffers.clear();

            msaa.destroy();
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

            for (auto framebuffer : swapChainFramebuffers) {
                vkDestroyFramebuffer(device, framebuffer, nullptr);
            }
            swapChainFramebuffers.clear();
            msaa.destroy();
            destroyDepthResources();
            destroyRenderFinishedSemaphores();

            const VkFormat oldFormat = swapchain.format();
            swapchain.recreate();
            camera.setAspectRatio(static_cast<float>(swapchain.extent().width) /
                                  static_cast<float>(swapchain.extent().height));
            msaa.create(swapchain.extent(), swapchain.format());
            createDepthResources();
            createRenderFinishedSemaphores();

            if (oldFormat != swapchain.format()) {
                destroySkyboxResources();
                graphicsPipeline.destroy();
                indexBuffer.destroy();
                vertexBuffer.destroy();
                for (Buffer& uniformBuffer : uniformBuffers) {
                    uniformBuffer.destroy();
                }
                createGraphicsPipeline();
                createSkyboxResources();
            }

            createFramebuffers();
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
            updateInstanceBuffer();
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
                snprintf(title, sizeof(title), "Vulkan + SDL3 - Cube | FPS: %.1f | Visible: %u/%zu",
                         fps, visibleCubeCount, Scene::CubeCount);
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

                destroySkyboxResources();
                graphicsPipeline.destroy();
                destroyShadowResources();
                indexBuffer.destroy();
                vertexBuffer.destroy();
                for (Buffer& buffer : instanceBuffers) {
                    buffer.destroy();
                }
                for (Buffer& buffer : shadowInstanceBuffers) {
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

void Renderer::run() {
    CubeApp app;
    app.run();
}

} // namespace Engine
