        void initWindow() const {
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
            depthBuffer.initialize(vulkanDevice.physical(), device, vulkanDevice.allocator());
            const VkSampleCountFlagBits requestedSamples =
                antialiasingLevel == AntialiasingLevel::MSAA4x ? VK_SAMPLE_COUNT_4_BIT :
                antialiasingLevel == AntialiasingLevel::MSAA2x ? VK_SAMPLE_COUNT_2_BIT :
                VK_SAMPLE_COUNT_1_BIT;
            msaa.initialize(vulkanDevice.physical(), device, requestedSamples, vulkanDevice.allocator());
            waitForDrawableExtent();
            createSwapChain();
            hdrBuffer.create(vulkanDevice.physical(), device, swapchain.extent(), vulkanDevice.allocator());
            msaa.create(swapchain.extent(), HdrBuffer::Format);
            createDepthResources();
            createCommandPool();
            createMaterialTextures();
            createMeshBuffers();
            renderableTopologySignature = currentRenderableTopologySignature();
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
            createTemporalAaPass();
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

        static std::vector<const char*> getRequiredExtensions(const bool useValidation) {
            Uint32 sdlExtensionCount = 0;
            const char * const *sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);
            if (sdlExtensions == nullptr) {
                throw std::runtime_error(std::string("SDL_Vulkan_GetInstanceExtensions error: ") + SDL_GetError());
            }
            std::vector<const char*> extensions(sdlExtensions, sdlExtensions + sdlExtensionCount);

            if (useValidation) {
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
            const bool validationSupported = checkValidationLayerSupport();
            const bool useValidation = validationSupported;

            if (!validationSupported) {
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

            const auto extensions = getRequiredExtensions(useValidation);
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

        // These parameters mirror the Vulkan extension function signature.
        // NOLINTBEGIN(bugprone-easily-swappable-parameters)
        static VkResult CreateDebugUtilsMessengerEXT(VkInstance instance,
            const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
            const VkAllocationCallbacks* pAllocator,
            VkDebugUtilsMessengerEXT* pDebugMessenger) {
            auto func = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
            if (func != nullptr) { return func(instance, pCreateInfo, pAllocator, pDebugMessenger); }
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
        // NOLINTEND(bugprone-easily-swappable-parameters)

        // These parameters mirror the Vulkan extension function signature.
        // NOLINTBEGIN(bugprone-easily-swappable-parameters)
        static void DestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const VkAllocationCallbacks* pAllocator) {
            if (const auto func = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT")); func != nullptr) { func(instance, debugMessenger, pAllocator); }
        }
        // NOLINTEND(bugprone-easily-swappable-parameters)

        void setupDebugMessenger() {
            if (!checkValidationLayerSupport()) { return; }
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

        [[nodiscard]] bool hasDrawableExtent() const {
            int width = 0;
            int height = 0;
            if (!SDL_GetWindowSizeInPixels(window, &width, &height) ||
                width <= 0 || height <= 0) {
                return false;
            }

            // During minimization SDL and the Vulkan surface can be briefly
            // out of sync.  The surface's currentExtent is authoritative;
            // it may be zero even while SDL still reports the old size.
            VkSurfaceCapabilitiesKHR capabilities{};
            if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
                    vulkanDevice.physical(), surface, &capabilities) != VK_SUCCESS) {
                return false;
            }
            return capabilities.currentExtent.width == UINT32_MAX ||
                   (capabilities.currentExtent.width > 0 &&
                    capabilities.currentExtent.height > 0);
        }

        void waitForDrawableExtent() const {
            while (!hasDrawableExtent()) {
                SDL_Event event;
                SDL_WaitEvent(&event);
            }
        }


        void createDepthResources() {
            depthBuffer.create(swapchain.extent(), msaa.sampleCount());
            hiZDepthBuffer.initialize(vulkanDevice.physical(), device, vulkanDevice.allocator());
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
                              sizeof(UniformBufferObject), vulkanDevice.allocator(), assetManager);
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
                                       sizeof(UniformBufferObject), vulkanDevice.allocator(), assetManager);
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
            if (!scene.isParticleScene() || scene.particleEntity() == NullEntity ||
                (!registry.has<ParticleEmitterComponent>(scene.particleEntity()) &&
                 !registry.has<SmokeEmitterComponent>(scene.particleEntity()))) {
                return;
            }

            if (!particleSystem) {
                particleSystem = std::make_unique<Particles::ParticleSystem>(
                    device, vulkanDevice.physical(), vulkanDevice.graphicsQueue(), commandPool, 8192);
                if (registry.has<SmokeEmitterComponent>(scene.particleEntity())) {
                    auto emitter = registry.get<SmokeEmitterComponent>(scene.particleEntity()).emitter;
                    if (registry.has<Transform>(scene.particleEntity())) { emitter.position = registry.get<Transform>(scene.particleEntity()).position;
}
                    if (registry.has<ColorPickerComponent>(scene.particleEntity())) { emitter.color = registry.get<ColorPickerComponent>(scene.particleEntity()).color;
}
                    particleSystem->setEmitter(emitter);
                } else {
                    auto emitter = registry.get<ParticleEmitterComponent>(scene.particleEntity()).emitter;
                    if (registry.has<Transform>(scene.particleEntity())) { emitter.position = registry.get<Transform>(scene.particleEntity()).position;
}
                    if (registry.has<ColorPickerComponent>(scene.particleEntity())) { emitter.color = registry.get<ColorPickerComponent>(scene.particleEntity()).color;
}
                    particleSystem->setEmitter(emitter);
                }
            }

            GraphicsPipelineOptions options{};
            options.colorFormat = HdrBuffer::Format;
            options.depthFormat = depthBuffer.format();
            options.samples = msaa.sampleCount();
            options.existingRenderPass = forwardPass.renderPass();
            options.shader = "shaders/particle_billboard.spv";
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
                const auto descriptorSetLayout = particleSystem->descriptorSetLayout();
                layout.setLayoutCount = 1;
                layout.pSetLayouts = &descriptorSetLayout;
                layout.pushConstantRangeCount = 1;
                layout.pPushConstantRanges = &pushConstants;
                if (vkCreatePipelineLayout(device, &layout, nullptr, &particleComputePipelineLayout) != VK_SUCCESS) {
                    throw std::runtime_error("Could not create particle compute pipeline layout");
                }
                particleComputePipeline = createComputePipeline(
                    "shaders/particle_simulation.spv", particleComputePipelineLayout);
            }
        }

        void reconfigureAntialiasing(const AntialiasingLevel requestedLevel) {
            if (device == VK_NULL_HANDLE) { return;
}

            // A minimized window reports a zero drawable extent. Waiting here
            // prevents recreating HDR attachments with that transient size.
            waitForDrawableExtent();
            antialiasingLevel = requestedLevel;
            taaSampleIndex = 0;

            // Nothing may reference the old render passes or attachments while
            // they are being replaced. This also guarantees that the old
            // command buffers have finished before their pipelines disappear.
            vkDeviceWaitIdle(device);

            destroyCullingResources();
            destroyEditorUiResources();
            canvasRenderer.destroy();
            tonemapPass.destroy();
            temporalAaPass.destroy();
            destroyVelocityResources();

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
            msaa.initialize(vulkanDevice.physical(), device, requestedSamples, vulkanDevice.allocator());
            hdrBuffer.create(vulkanDevice.physical(), device, swapchain.extent(), vulkanDevice.allocator());
            msaa.create(swapchain.extent(), HdrBuffer::Format);
            createDepthResources();

            createForwardPass();
            createParticleResources();
            createSkyPass();
            createSceneSkyPass();
            createFramebuffers();
            createSceneViewportResources();
            createTemporalAaPass();
            createTonemapPass();
            createUIResources();
            createEditorUiResources();
            createCullingResources();
        }

        void createSkyPass() const {
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

        void createTonemapPass() const {
            tonemapPass.create(device, swapchain.format(), swapchain.extent(),
                               swapchain.imageViews(), hdrBuffer.imageView(),
                               hdrBuffer.sampler(), assetManager, temporalAaPass.historyViews());
        }

        void createTemporalAaPass() {
            if (antialiasingLevel != AntialiasingLevel::TAA) return;
            temporalAaPass.create(vulkanDevice.physical(), device, swapchain.extent(),
                                  vulkanDevice.allocator(), hdrBuffer.imageView(),
                                  hdrBuffer.sampler(), velocityBuffer.imageView(),
                                  velocityBuffer.sampler(), assetManager);
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
            if (ImGui::GetCurrentContext() == nullptr) { return; }
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
                const auto view = swapchain.imageViews()[index];
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
            constexpr uint32_t imguiDescriptorPoolSize{128};
            info.DescriptorPoolSize = imguiDescriptorPoolSize; info.MinImageCount = 2;
            info.ImageCount = static_cast<uint32_t>(swapchain.imageCount());
            info.PipelineInfoMain.RenderPass = editorUiRenderPass;
            info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
            if (!ImGui_ImplVulkan_Init(&info)) throw std::runtime_error("Could not initialize ImGui Vulkan backend");
            gameViewportDescriptor = ImGui_ImplVulkan_AddTexture(hdrBuffer.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            if (antialiasingLevel == AntialiasingLevel::TAA) {
                const auto historyViews = temporalAaPass.historyViews();
                for (std::size_t index = 0; index < historyViews.size(); ++index) {
                    gameViewportTemporalDescriptors[index] = ImGui_ImplVulkan_AddTexture(
                        historyViews[index], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }
            }
            sceneViewportDescriptor = ImGui_ImplVulkan_AddTexture(sceneViewportTarget.color().imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            editorUiActive = true;
        }

        void destroyEditorUiResources() noexcept {
            if (editorUiActive) {
                if (gameViewportDescriptor != VK_NULL_HANDLE) { ImGui_ImplVulkan_RemoveTexture(gameViewportDescriptor);
}
                for (const VkDescriptorSet descriptor : gameViewportTemporalDescriptors) {
                    if (descriptor != VK_NULL_HANDLE) ImGui_ImplVulkan_RemoveTexture(descriptor);
                }
                if (sceneViewportDescriptor != VK_NULL_HANDLE) { ImGui_ImplVulkan_RemoveTexture(sceneViewportDescriptor);
}
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
            gameViewportTemporalDescriptors.fill(VK_NULL_HANDLE);
            editorUiActive = false;
            for (VkFramebuffer framebuffer : editorUiFramebuffers) if (framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device, framebuffer, nullptr);
            editorUiFramebuffers.clear();
            if (editorUiRenderPass != VK_NULL_HANDLE) { vkDestroyRenderPass(device, editorUiRenderPass, nullptr);
}
            editorUiRenderPass = VK_NULL_HANDLE;
        }

        // Scene reload replaces the images displayed by ImGui::Image. Rebind
        // its descriptors before the next UI command buffer is recorded.
        void refreshEditorViewportTextures() {
            if (!editorUiActive) { return;
}
            if (gameViewportDescriptor != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(gameViewportDescriptor);
            }
            for (const VkDescriptorSet descriptor : gameViewportTemporalDescriptors) {
                if (descriptor != VK_NULL_HANDLE) ImGui_ImplVulkan_RemoveTexture(descriptor);
            }
            gameViewportTemporalDescriptors.fill(VK_NULL_HANDLE);
            if (sceneViewportDescriptor != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(sceneViewportDescriptor);
            }
            gameViewportDescriptor = ImGui_ImplVulkan_AddTexture(
                hdrBuffer.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            if (antialiasingLevel == AntialiasingLevel::TAA) {
                const auto historyViews = temporalAaPass.historyViews();
                for (std::size_t index = 0; index < historyViews.size(); ++index) {
                    gameViewportTemporalDescriptors[index] = ImGui_ImplVulkan_AddTexture(
                        historyViews[index], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }
            }
            sceneViewportDescriptor = ImGui_ImplVulkan_AddTexture(
                sceneViewportTarget.color().imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
