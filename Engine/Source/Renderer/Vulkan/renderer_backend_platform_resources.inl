        void initWindow() const {
            if (!window) {
                throw std::invalid_argument("Renderer requires an application-owned SDL window");
            }
        }

        void initVulkanCore() {
            const char* basePath = SDL_GetBasePath();
            assetManager.set_asset_root(basePath ? std::filesystem::path(basePath) : std::filesystem::path{});
            Assets::register_default_asset_loaders(assetManager);
            assetManager.set_error_handler([](const std::string& message) { std::cerr << "[Assets] " << message << '\n'; });
            createInstance();
            setupDebugMessenger();
            createSurface();
            vulkanDevice.create(instance, surface);
            device = vulkanDevice.logical();
            if (vulkanDevice.supportsRayQuery()) accelerationStructures.create(vulkanDevice.physical(), device, vulkanDevice.allocator());
            gpuTimestampProfiler.create(vulkanDevice.physical(), device);
            waitForDrawableExtent();
            createSwapChain();
            createCommandPool();
            uploadContext.create(device, vulkanDevice.transferQueue(), vulkanDevice.transferQueueFamily(),
                                 vulkanDevice.graphicsQueue(), vulkanDevice.graphicsQueueFamily(),
                                 vulkanDevice.computeQueueFamily(),
                                 vulkanDevice.allocator());
            UploadContext::setCurrent(&uploadContext);
            createCommandBuffers();
            createSyncObjects();
            createEditorUiResources(false);
        }

        void createImageBasedLighting() {
            // SDL's base path is the executable directory. Development
            // projects use Assets, while packaged builds use Content.
            const auto runtimeRoot = projectRoot.empty() ? assetManager.asset_root() : projectRoot;
            const auto contentDirectory = runtimeRoot / "Content";
            const auto assetDirectory = std::filesystem::is_directory(contentDirectory)
                ? contentDirectory
                : runtimeRoot / "Assets";
            const auto hdrEnvironment = assetDirectory / "Environment.hdr";
            const auto exrEnvironment = assetDirectory / "Environment.exr";
            const auto sceneEnvironment = scene.environmentEquirectangular();
            auto sceneEnvironmentPath = sceneEnvironment;
            // Older scenes stored the source tree's "Assets/" prefix. The
            // serialized environment path is asset-root-relative, so accept
            // that legacy form while resolving both development and packaged
            // layouts.
            if (!sceneEnvironmentPath.empty() && sceneEnvironmentPath.is_relative()) {
                const auto first = sceneEnvironmentPath.begin();
                if (first != sceneEnvironmentPath.end() &&
                    (first->generic_string() == "Assets" || first->generic_string() == "Content")) {
                    sceneEnvironmentPath = sceneEnvironmentPath.lexically_relative(*first);
                }
            }
            const auto environmentPath = !environmentEquirectangularPath.empty()
                ? environmentEquirectangularPath
                : sceneEnvironmentPath.empty()
                ? (std::filesystem::exists(hdrEnvironment) ? hdrEnvironment
                    : std::filesystem::exists(exrEnvironment) ? exrEnvironment
                    : std::filesystem::path{})
                : sceneEnvironmentPath.is_absolute() ? sceneEnvironmentPath : assetDirectory / sceneEnvironmentPath;
            imageBasedLighting.create(vulkanDevice.physical(), device, commandPool,
                                      vulkanDevice.graphicsQueue(), vulkanDevice.allocator(),
                                      environmentPath, runtimeRoot / "Library", iblQualitySettings(iblQuality));
        }

        void initSceneResources() {
            const auto timeInitialization = [](const std::string_view stage, const auto& initialize) {
                const auto startedAt = std::chrono::steady_clock::now();
                initialize();
                const auto elapsed = std::chrono::steady_clock::now() - startedAt;
                const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
                Diagnostics::instance().report(
                    DiagnosticSeverity::Info,
                    "[GPUInit] " + std::string{stage} + ": " + std::to_string(milliseconds) + " ms",
                    {.subsystem = "Renderer"});
            };

            timeInitialization("Depth", [&] {
                GE_PROFILE_SCOPE("Renderer.RenderTargets");
                depthBuffer.initialize(vulkanDevice.physical(), device, vulkanDevice.allocator());
                const VkSampleCountFlagBits requestedSamples =
                    antialiasingLevel == AntialiasingLevel::MSAA4x ? VK_SAMPLE_COUNT_4_BIT :
                    antialiasingLevel == AntialiasingLevel::MSAA2x ? VK_SAMPLE_COUNT_2_BIT :
                    VK_SAMPLE_COUNT_1_BIT;
                msaa.initialize(vulkanDevice.physical(), device, requestedSamples, vulkanDevice.allocator());
                hdrBuffer.create(vulkanDevice.physical(), device, swapchain.extent(), vulkanDevice.allocator());
                hdrBufferInitialized = false;
                opaqueSceneColor.create(vulkanDevice.physical(), device, swapchain.extent(), vulkanDevice.allocator());
                opaqueSceneColorInitialized = false;
                msaa.create(swapchain.extent(), HdrBuffer::Format);
                createDepthResources();
            });
            timeInitialization("GTAO", [&] { GE_PROFILE_SCOPE("Renderer.GTAO"); createGtaoPass(); });
            timeInitialization("Material textures", [&] {
                GE_PROFILE_SCOPE("Renderer.MaterialTextures");
                createMaterialTextures();
            });
            timeInitialization("IBL", [&] { GE_PROFILE_SCOPE("Renderer.IBL"); createImageBasedLighting(); });
            timeInitialization("Mesh buffers", [&] { GE_PROFILE_SCOPE("Renderer.MeshUpload"); createMeshBuffers(); });
            lastRenderTopologyRevision = registry.renderTopologyRevision();
            lastParticleEmitterRevision = registry.componentRevision<ParticleEmitterComponent>();
            lastSmokeEmitterRevision = registry.componentRevision<SmokeEmitterComponent>();
            timeInitialization("Instance buffer", [&] {
                GE_PROFILE_SCOPE("Renderer.InstanceBuffers");
                createInstanceBuffer();
            });
            timeInitialization("Uniform buffers", [&] {
                GE_PROFILE_SCOPE("Renderer.UniformBuffers");
                createUniformBuffers();
            });
            timeInitialization("Scene uniform buffers", [&] {
                GE_PROFILE_SCOPE("Renderer.UniformBuffers");
                createSceneUniformBuffers();
            });
            timeInitialization("Culling", [&] { GE_PROFILE_SCOPE("Renderer.Culling"); createCullingResources(); });
            timeInitialization("Shadows", [&] { GE_PROFILE_SCOPE("Renderer.Shadows"); createShadowPass(); });
            timeInitialization("Scene descriptors", [&] {
                GE_PROFILE_SCOPE("Renderer.Pipelines");
                createSceneDescriptorPass();
            });
            timeInitialization("Forward pipelines", [&] {
                GE_PROFILE_SCOPE("Renderer.Pipelines");
                createForwardPass();
            });
            timeInitialization("Sky", [&] {
                GE_PROFILE_SCOPE("Renderer.Pipelines");
                createSkyPass();
            });
            timeInitialization("Framebuffers", [&] {
                GE_PROFILE_SCOPE("Renderer.Pipelines");
                createFramebuffers();
            });
            timeInitialization("Scene viewport", [&] {
                GE_PROFILE_SCOPE("Renderer.Pipelines");
                createSceneViewportResources();
            });
            timeInitialization("Particles", [&] {
                GE_PROFILE_SCOPE("Renderer.Pipelines");
                createParticleResources();
            });
            timeInitialization("Scene sky", [&] {
                GE_PROFILE_SCOPE("Renderer.Pipelines");
                createSceneSkyPass();
            });
            timeInitialization("TAA", [&] {
                GE_PROFILE_SCOPE("Renderer.PostProcess");
                createTemporalAaPass();
            });
            timeInitialization("Bloom", [&] {
                GE_PROFILE_SCOPE("Renderer.PostProcess");
                createBloomPass();
            });
            timeInitialization("Tonemap", [&] {
                GE_PROFILE_SCOPE("Renderer.PostProcess");
                createTonemapPass();
            });
            timeInitialization("UI resources", [&] {
                GE_PROFILE_SCOPE("Renderer.EditorResources");
                createUIResources();
            });
            timeInitialization("Editor viewport textures", [&] {
                GE_PROFILE_SCOPE("Renderer.EditorResources");
                refreshEditorViewportTextures();
            });
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
                const auto severity = messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT
                                          ? DiagnosticSeverity::Error
                                          : DiagnosticSeverity::Warning;
                Diagnostics::instance().report(
                    severity,
                    std::string{"[Vulkan] "} +
                        (pCallbackData != nullptr && pCallbackData->pMessage != nullptr
                             ? pCallbackData->pMessage
                             : "Validation layer returned an empty message."),
                    {.subsystem = "Vulkan"});
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
            const bool useValidation = enableValidationLayers && validationSupported;

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
            if (!enableValidationLayers || !checkValidationLayerSupport()) { return; }
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
            const std::array depthFamilies{
                vulkanDevice.graphicsQueueFamily(), vulkanDevice.computeQueueFamily()};
            const std::span<const std::uint32_t> sharingFamilies =
                vulkanDevice.hasAsyncComputeQueue() && depthFamilies[0] != depthFamilies[1]
                    ? std::span<const std::uint32_t>(depthFamilies)
                    : std::span<const std::uint32_t>{};
            depthBuffer.create(swapchain.extent(), msaa.sampleCount(), VK_FORMAT_UNDEFINED,
                               sharingFamilies);
            sceneViewportDepthBuffer.initialize(vulkanDevice.physical(), device, vulkanDevice.allocator());
            hiZDepthBuffer.initialize(vulkanDevice.physical(), device, vulkanDevice.allocator());
            if (msaa.enabled()) {
                hiZDepthBuffer.create(swapchain.extent(), VK_SAMPLE_COUNT_1_BIT,
                                      depthBuffer.format(), sharingFamilies);
            }
        }

        void destroyDepthResources() {
            sceneViewportDepthBuffer.destroy();
            hiZDepthBuffer.destroy();
            depthBuffer.destroy();
        }

        void createGtaoPass() {
            const std::array gtaoFamilies{
                vulkanDevice.graphicsQueueFamily(), vulkanDevice.computeQueueFamily()};
            const std::span<const std::uint32_t> sharingFamilies =
                vulkanDevice.hasAsyncComputeQueue() && gtaoFamilies[0] != gtaoFamilies[1]
                    ? std::span<const std::uint32_t>(gtaoFamilies)
                    : std::span<const std::uint32_t>{};
            gtaoPass.create(vulkanDevice.physical(), device, commandPool, vulkanDevice.graphicsQueue(), swapchain.extent(),
                            vulkanDevice.allocator(), assetManager, gtaoQualitySettings(gtaoQuality), sharingFamilies);
        }

        void createRtContactShadowPass() {
            if (!vulkanDevice.supportsRayQuery() || msaa.enabled()) return;
            const float scale = std::clamp(rtContactShadowSettings.resolutionScale, 0.25F, 1.0F);
            const VkExtent2D fullExtent = swapchain.extent();
            const VkExtent2D targetExtent{
                std::max(1u, static_cast<std::uint32_t>(float(fullExtent.width) * scale)),
                std::max(1u, static_cast<std::uint32_t>(float(fullExtent.height) * scale))};
            if (rtContactShadowPass.resultView(0) != VK_NULL_HANDLE &&
                rtContactShadowPass.extent().width == targetExtent.width &&
                rtContactShadowPass.extent().height == targetExtent.height) return;
            // A resolution change replaces descriptors/images shared by both
            // in-flight slots. This happens only after a UI setting change or
            // resize, so retire all old users before destroying them.
            if (rtContactShadowPass.resultView(0) != VK_NULL_HANDLE &&
                vkDeviceWaitIdle(device) != VK_SUCCESS)
                throw std::runtime_error("Could not idle device to resize RT contact shadows");
            std::array<VkBuffer, MAX_FRAMES_IN_FLIGHT> buffers{};
            for (std::uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame)
                buffers[frame] = uniformBuffers[frame].handle();
            rtContactShadowPass.create(vulkanDevice.physical(), device, targetExtent,
                                       vulkanDevice.allocator(), assetManager, buffers);
        }

        void createDirectionalVisibilityPass() {
            if (msaa.enabled()) return;
            const VkExtent2D targetExtent = swapchain.extent();
            if (directionalVisibilityPass.resultView(0) != VK_NULL_HANDLE &&
                directionalVisibilityPass.extent().width == targetExtent.width &&
                directionalVisibilityPass.extent().height == targetExtent.height) return;
            if (directionalVisibilityPass.resultView(0) != VK_NULL_HANDLE &&
                vkDeviceWaitIdle(device) != VK_SUCCESS)
                throw std::runtime_error("Could not idle device to resize directional visibility");
            std::array<VkBuffer, MAX_FRAMES_IN_FLIGHT> frames{};
            std::array<VkBuffer, MAX_FRAMES_IN_FLIGHT> pages{};
            for (std::uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
                frames[frame] = uniformBuffers[frame].handle();
                pages[frame] = shadowPass.pageTableBuffer(frame);
            }
            directionalVisibilityPass.create(vulkanDevice.physical(), device, targetExtent,
                vulkanDevice.allocator(), assetManager, physicalShadowPagePool, frames, pages);
        }

        void bindGtaoTexture(ShadowPass& descriptors) {
            const VkDescriptorImageInfo gtao{gtaoPass.resultSampler(), gtaoPass.resultView(),
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            for (std::uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame)
                descriptors.setGtaoTexture(frame, gtao);
        }

        void bindContactShadowFallback(ShadowPass& descriptors) {
            const VkDescriptorImageInfo white{fallbackMaterialTexture.sampler(), fallbackMaterialTexture.imageView(),
                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            for (std::uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame)
                descriptors.setContactShadowTexture(frame, white);
        }

        void createShadowPass() {
            std::vector<VkBuffer> buffers;
            std::vector<VkBuffer> gpuMaterialBuffers;
            std::vector<VkBuffer> gpuInstanceBuffers;
            std::vector<VkBuffer> gpuPreviousTransformBuffers;
            std::vector<VkBuffer> gpuInstanceIndexBuffers;
            std::vector<VkBuffer> gpuGrassInstanceBuffers;
            std::vector<VkBuffer> gpuGrassClusterBuffers;
            std::vector<VkBuffer> gpuGrassDeformationBuffers;
            std::vector<VkBuffer> clusterRangeBuffers;
            std::vector<VkBuffer> clusterIndexBuffers;
            std::vector<VkBuffer> reflectionBuffers;
            buffers.reserve(uniformBuffers.size());
            gpuMaterialBuffers.reserve(materialBuffers.size());
            gpuInstanceBuffers.reserve(instanceBuffers.size());
            gpuPreviousTransformBuffers.reserve(previousTransformBuffers.size());
            gpuInstanceIndexBuffers.reserve(compactGrassInstanceBuffers.size());
            gpuGrassInstanceBuffers.reserve(generatedGrassInstanceBuffers.size());
            gpuGrassClusterBuffers.reserve(grassClusterBuffers.size());
            gpuGrassDeformationBuffers.reserve(grassDeformationBuffers.size());
            clusterRangeBuffers.reserve(clusteredLightRangeBuffers.size());
            clusterIndexBuffers.reserve(clusteredLightIndexBuffers.size());
            reflectionBuffers.reserve(reflectionProbeBuffers.size());
            for (const Buffer& buffer : uniformBuffers) {
                buffers.push_back(buffer.handle());
            }
            for (const Buffer& buffer : materialBuffers) {
                gpuMaterialBuffers.push_back(buffer.handle());
            }
            for (const Buffer& buffer : instanceBuffers) gpuInstanceBuffers.push_back(buffer.handle());
            for (std::size_t i = 0; i < previousTransformBuffers.size(); ++i)
                gpuPreviousTransformBuffers.push_back(previousTransformBuffers[i].handle() != VK_NULL_HANDLE
                    ? previousTransformBuffers[i].handle() : instanceBuffers[i].handle());
            for (const Buffer& buffer : compactGrassInstanceBuffers) gpuInstanceIndexBuffers.push_back(buffer.handle());
            for (std::size_t i = 0; i < generatedGrassInstanceBuffers.size(); ++i)
                gpuGrassInstanceBuffers.push_back(generatedGrassInstanceBuffers[i].handle() != VK_NULL_HANDLE
                    ? generatedGrassInstanceBuffers[i].handle() : instanceBuffers[i].handle());
            for (std::size_t i = 0; i < grassClusterBuffers.size(); ++i)
                gpuGrassClusterBuffers.push_back(grassClusterBuffers[i].handle() != VK_NULL_HANDLE
                    ? grassClusterBuffers[i].handle() : instanceBuffers[i].handle());
            for (std::size_t i = 0; i < grassDeformationBuffers.size(); ++i)
                gpuGrassDeformationBuffers.push_back(grassDeformationBuffers[i].handle() != VK_NULL_HANDLE
                    ? grassDeformationBuffers[i].handle() : instanceBuffers[i].handle());
            for (const Buffer& buffer : clusteredLightRangeBuffers) clusterRangeBuffers.push_back(buffer.handle());
            for (const Buffer& buffer : clusteredLightIndexBuffers) clusterIndexBuffers.push_back(buffer.handle());
            for (const Buffer& buffer : reflectionProbeBuffers) reflectionBuffers.push_back(buffer.handle());
            if (shadowPass.descriptorSetLayout() == VK_NULL_HANDLE) {
                shadowPass.create(vulkanDevice.physical(), device, physicalShadowPagePool, buffers,
                                  gpuMaterialBuffers, gpuInstanceBuffers, gpuPreviousTransformBuffers, gpuInstanceIndexBuffers,
                                  gpuGrassInstanceBuffers, gpuGrassClusterBuffers, gpuGrassDeformationBuffers,
                                  clusterRangeBuffers, clusterIndexBuffers, reflectionBuffers,
                                  materialTextureDescriptors, imageBasedLighting.descriptors(), sizeof(UniformBufferObject),
                                  vulkanDevice.allocator(), assetManager);
            } else {
                shadowPass.updateDescriptors(
                    buffers, gpuMaterialBuffers, gpuInstanceBuffers, gpuPreviousTransformBuffers, gpuInstanceIndexBuffers,
                    gpuGrassInstanceBuffers, gpuGrassClusterBuffers, gpuGrassDeformationBuffers,
                    clusterRangeBuffers, clusterIndexBuffers, reflectionBuffers,
                    materialTextureDescriptors, imageBasedLighting.descriptors(), sizeof(UniformBufferObject));
            }
            bindGtaoTexture(shadowPass);
            bindContactShadowFallback(shadowPass);
            createDirectionalVisibilityPass();
            createRtContactShadowPass();
            if (vulkanDevice.supportsRayQuery()) {
                ddgi.create(vulkanDevice.physical(), device, vulkanDevice.allocator(),
                            shadowPass.descriptorSetLayout(), assetManager);
                for (std::uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
                    std::array<VkDescriptorImageInfo, 9> textures{};
                    for (std::uint32_t cascade = 0; cascade < 3; ++cascade) {
                        const auto& probes = ddgi.resources().cascades[cascade].history;
                        textures[cascade * 3] = {probes.irradiance.sampler(),
                            probes.irradiance.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                        textures[cascade * 3 + 1] = {probes.distance.sampler(),
                            probes.distance.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                        textures[cascade * 3 + 2] = {probes.probeData.sampler(),
                            probes.probeData.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                    }
                    shadowPass.setDDGITextures(frame, textures);
                }
            }
        }

        void createSceneDescriptorPass() {
            std::vector<VkBuffer> buffers;
            std::vector<VkBuffer> gpuMaterialBuffers;
            std::vector<VkBuffer> gpuInstanceBuffers;
            std::vector<VkBuffer> gpuPreviousTransformBuffers;
            std::vector<VkBuffer> gpuInstanceIndexBuffers;
            std::vector<VkBuffer> gpuGrassInstanceBuffers;
            std::vector<VkBuffer> gpuGrassClusterBuffers;
            std::vector<VkBuffer> gpuGrassDeformationBuffers;
            std::vector<VkBuffer> clusterRangeBuffers;
            std::vector<VkBuffer> clusterIndexBuffers;
            std::vector<VkBuffer> reflectionBuffers;
            buffers.reserve(sceneUniformBuffers.size());
            gpuMaterialBuffers.reserve(materialBuffers.size());
            gpuInstanceBuffers.reserve(instanceBuffers.size());
            gpuPreviousTransformBuffers.reserve(previousTransformBuffers.size());
            gpuInstanceIndexBuffers.reserve(compactGrassInstanceBuffers.size());
            gpuGrassInstanceBuffers.reserve(generatedGrassInstanceBuffers.size());
            gpuGrassClusterBuffers.reserve(grassClusterBuffers.size());
            gpuGrassDeformationBuffers.reserve(grassDeformationBuffers.size());
            clusterRangeBuffers.reserve(sceneClusteredLightRangeBuffers.size());
            clusterIndexBuffers.reserve(sceneClusteredLightIndexBuffers.size());
            reflectionBuffers.reserve(reflectionProbeBuffers.size());
            for (const Buffer& buffer : sceneUniformBuffers) buffers.push_back(buffer.handle());
            for (const Buffer& buffer : materialBuffers) gpuMaterialBuffers.push_back(buffer.handle());
            for (const Buffer& buffer : instanceBuffers) gpuInstanceBuffers.push_back(buffer.handle());
            for (std::size_t i = 0; i < previousTransformBuffers.size(); ++i)
                gpuPreviousTransformBuffers.push_back(previousTransformBuffers[i].handle() != VK_NULL_HANDLE
                    ? previousTransformBuffers[i].handle() : instanceBuffers[i].handle());
            for (const Buffer& buffer : compactGrassInstanceBuffers) gpuInstanceIndexBuffers.push_back(buffer.handle());
            for (std::size_t i = 0; i < generatedGrassInstanceBuffers.size(); ++i)
                gpuGrassInstanceBuffers.push_back(generatedGrassInstanceBuffers[i].handle() != VK_NULL_HANDLE
                    ? generatedGrassInstanceBuffers[i].handle() : instanceBuffers[i].handle());
            for (std::size_t i = 0; i < grassClusterBuffers.size(); ++i)
                gpuGrassClusterBuffers.push_back(grassClusterBuffers[i].handle() != VK_NULL_HANDLE
                    ? grassClusterBuffers[i].handle() : instanceBuffers[i].handle());
            for (std::size_t i = 0; i < grassDeformationBuffers.size(); ++i)
                gpuGrassDeformationBuffers.push_back(grassDeformationBuffers[i].handle() != VK_NULL_HANDLE
                    ? grassDeformationBuffers[i].handle() : instanceBuffers[i].handle());
            for (const Buffer& buffer : sceneClusteredLightRangeBuffers) clusterRangeBuffers.push_back(buffer.handle());
            for (const Buffer& buffer : sceneClusteredLightIndexBuffers) clusterIndexBuffers.push_back(buffer.handle());
            for (const Buffer& buffer : reflectionProbeBuffers) reflectionBuffers.push_back(buffer.handle());
            if (sceneDescriptorPass.descriptorSetLayout() == VK_NULL_HANDLE) {
                sceneDescriptorPass.create(vulkanDevice.physical(), device, physicalShadowPagePool, buffers,
                                           gpuMaterialBuffers, gpuInstanceBuffers, gpuPreviousTransformBuffers, gpuInstanceIndexBuffers,
                                           gpuGrassInstanceBuffers, gpuGrassClusterBuffers, gpuGrassDeformationBuffers,
                                           clusterRangeBuffers, clusterIndexBuffers, reflectionBuffers,
                                           materialTextureDescriptors, imageBasedLighting.descriptors(), sizeof(UniformBufferObject),
                                           vulkanDevice.allocator(), assetManager);
            } else {
                sceneDescriptorPass.updateDescriptors(
                    buffers, gpuMaterialBuffers, gpuInstanceBuffers, gpuPreviousTransformBuffers, gpuInstanceIndexBuffers,
                    gpuGrassInstanceBuffers, gpuGrassClusterBuffers, gpuGrassDeformationBuffers,
                    clusterRangeBuffers, clusterIndexBuffers, reflectionBuffers,
                    materialTextureDescriptors, imageBasedLighting.descriptors(), sizeof(UniformBufferObject));
            }
            bindGtaoTexture(sceneDescriptorPass);
            bindContactShadowFallback(sceneDescriptorPass);
        }

        void createForwardPass() {
            forwardPass.create(device, HdrBuffer::Format, depthBuffer.format(),
                               msaa.sampleCount(),
                               msaa.enabled() ? hiZDepthBuffer.format() : VK_FORMAT_UNDEFINED,
                               msaa.enabled() ? vulkanDevice.depthResolveMode() : VK_RESOLVE_MODE_NONE,
                               shadowPass.descriptorSetLayout(), assetManager,
                               VK_IMAGE_LAYOUT_UNDEFINED, false,
                               !msaa.enabled() ? VK_FORMAT_R16G16_SFLOAT : VK_FORMAT_UNDEFINED,
                               !msaa.enabled() ? VK_FORMAT_R16G16_SNORM : VK_FORMAT_UNDEFINED, false, true);
            lightingForwardPass.create(device, HdrBuffer::Format, depthBuffer.format(),
                               msaa.sampleCount(),
                               msaa.enabled() ? hiZDepthBuffer.format() : VK_FORMAT_UNDEFINED,
                               msaa.enabled() ? vulkanDevice.depthResolveMode() : VK_RESOLVE_MODE_NONE,
                               shadowPass.descriptorSetLayout(), assetManager,
                               VK_IMAGE_LAYOUT_UNDEFINED, false,
                               antialiasingLevel == AntialiasingLevel::TAA ? VK_FORMAT_R16G16_SFLOAT : VK_FORMAT_UNDEFINED,
                               VK_FORMAT_UNDEFINED, true);
            waterPass.create(device, HdrBuffer::Format, depthBuffer.format(), msaa.sampleCount(),
                             msaa.enabled() ? hiZDepthBuffer.format() : VK_FORMAT_UNDEFINED,
                             msaa.enabled() ? vulkanDevice.depthResolveMode() : VK_RESOLVE_MODE_NONE,
                             shadowPass.descriptorSetLayout(), assetManager,
                             antialiasingLevel == AntialiasingLevel::TAA,
                             {opaqueSceneColor.sampler(), opaqueSceneColor.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                             {(msaa.enabled() ? hiZDepthBuffer : depthBuffer).sampler(),
                              (msaa.enabled() ? hiZDepthBuffer : depthBuffer).imageView(),
                              VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL});
        }

        void createSceneViewportForwardPass() {
            if (msaa.enabled()) return;
            sceneViewportForwardPass.create(
                device, HdrBuffer::Format, depthBuffer.format(),
                VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_UNDEFINED, VK_RESOLVE_MODE_NONE,
                sceneDescriptorPass.descriptorSetLayout(), assetManager,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true,
                VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, false);
        }

        void createParticleResources() {
            if (!scene.isParticleScene() || scene.particleEntity() == NullEntity ||
                (!registry.has<ParticleEmitterComponent>(scene.particleEntity()) &&
                 !registry.has<SmokeEmitterComponent>(scene.particleEntity()))) {
                return;
            }

            if (!particleSystem) {
                particleSystem = std::make_unique<Particles::ParticleSystem>(
                    device, vulkanDevice.allocator(), vulkanDevice.graphicsQueue(), commandPool, 8192);
                if (registry.has<SmokeEmitterComponent>(scene.particleEntity())) {
                    auto emitter = registry.get<SmokeEmitterComponent>(scene.particleEntity()).emitter;
                    if (registry.has<Transform>(scene.particleEntity())) { emitter.position = registry.get<Transform>(scene.particleEntity()).position;
}
                    particleSystem->setEmitter(emitter);
                } else {
                    auto emitter = registry.get<ParticleEmitterComponent>(scene.particleEntity()).emitter;
                    if (registry.has<Transform>(scene.particleEntity())) { emitter.position = registry.get<Transform>(scene.particleEntity()).position;
}
                    particleSystem->setEmitter(emitter);
                }
            }

            GraphicsPipelineOptions options{};
            options.colorFormat = HdrBuffer::Format;
            options.dynamicRendering = true;
            options.depthFormat = depthBuffer.format();
            options.samples = msaa.sampleCount();
            options.additionalColorFormat = antialiasingLevel == AntialiasingLevel::TAA
                ? VK_FORMAT_R16G16_SFLOAT : VK_FORMAT_UNDEFINED;
            options.shader = antialiasingLevel == AntialiasingLevel::TAA
                ? "shaders/particle_billboard.spv" : "shaders/particle_billboard_no_velocity.spv";
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

            // Dynamic rendering makes this pipeline independent from the
            // target used by the Game or Scene View.
            options.dynamicRendering = true;
            options.shader = "shaders/particle_billboard_no_velocity.spv";
            sceneParticlePipeline.create(device, options);

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
            if (!sceneResourcesInitialized) {
                antialiasingLevel = requestedLevel;
                return;
            }

            // A minimized window reports a zero drawable extent. Waiting here
            // prevents recreating HDR attachments with that transient size.
            waitForDrawableExtent();
            antialiasingLevel = requestedLevel;
            taaSampleIndex = 0;

            // Retire only frames and uploads which can reference these
            // attachments; do not idle unrelated queues during live resize.
            waitForGlobalResourceRebuild();

            // These descriptor sets bind packed grass buffers, which are
            // recreated together with culling resources below.
            virtualWaterRenderer.destroy();
            forwardPass.destroy();
            lightingForwardPass.destroy();
            waterPass.destroy();
            directionalVisibilityPass.destroy();
            ddgi.destroy();
            shadowPass.destroy();
            sceneDescriptorPass.destroy();
            destroyCullingResources();
            destroyEditorUiResources();
            canvasRenderer.destroy();
            tonemapPass.destroy();
            temporalAaPass.destroy();
            bloomPass.destroy();
            gtaoPass.destroy();
            destroyVelocityResources();

            skyPass.destroy();
            sceneSkyPass.destroy();
            destroySceneViewportResources();

            particlePipeline.destroy();
            sceneParticlePipeline.destroy();

            msaa.destroy();
            hdrBuffer.destroy();
            hdrBufferInitialized = false;
            opaqueSceneColor.destroy();
            opaqueSceneColorInitialized = false;
            destroyDepthResources();

            const VkSampleCountFlagBits requestedSamples =
                antialiasingLevel == AntialiasingLevel::MSAA4x ? VK_SAMPLE_COUNT_4_BIT :
                antialiasingLevel == AntialiasingLevel::MSAA2x ? VK_SAMPLE_COUNT_2_BIT :
                VK_SAMPLE_COUNT_1_BIT;
            msaa.initialize(vulkanDevice.physical(), device, requestedSamples, vulkanDevice.allocator());
            hdrBuffer.create(vulkanDevice.physical(), device, swapchain.extent(), vulkanDevice.allocator());
            hdrBufferInitialized = false;
            opaqueSceneColor.create(vulkanDevice.physical(), device, swapchain.extent(), vulkanDevice.allocator());
            opaqueSceneColorInitialized = false;
            msaa.create(swapchain.extent(), HdrBuffer::Format);
            createDepthResources();
            createGtaoPass();
            configuredGtaoQuality = gtaoQuality;

            // Instance buffers outlive swapchain attachments, so changing
            // AA mode must explicitly add/remove the TAA-only history stream.
            for (Buffer& buffer : previousTransformBuffers) buffer.destroy();
            if (antialiasingLevel == AntialiasingLevel::TAA) {
                const bool ownsPreviousTransformUploadBatch = !uploadContext.recording();
                if (ownsPreviousTransformUploadBatch) uploadContext.begin();
                for (Buffer& buffer : previousTransformBuffers) {
                    buffer.createDeviceLocalEmpty(device,
                        sizeof(RendererPreviousTransformData) *
                            std::max<std::size_t>(1, previousInstanceTransforms.size()),
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vulkanDevice.allocator());
                    if (!previousInstanceTransforms.empty()) {
                        buffer.uploadDeviceLocal(previousInstanceTransforms.data(),
                            sizeof(RendererPreviousTransformData) * previousInstanceTransforms.size(),
                            0, commandPool, vulkanDevice.graphicsQueue());
                    }
                }
                if (ownsPreviousTransformUploadBatch) {
                    [[maybe_unused]] const UploadTicket ticket = uploadContext.submit();
                }
            }

            createCullingResources();
            createShadowPass();
            createSceneDescriptorPass();
            createForwardPass();
            createSkyPass();
            createFramebuffers();
            createSceneViewportResources();
            createParticleResources();
            createSceneSkyPass();
            createTemporalAaPass();
            createBloomPass();
            createTonemapPass();
            createUIResources();
            createEditorUiResources();
            sceneViewportCacheValid = false;
            sceneViewportImageInitialized = false;
            sceneViewportNeedsRender = true;
        }

        void createSkyPass() const {
            std::vector<VkBuffer> buffers;
            buffers.reserve(uniformBuffers.size());
            for (const Buffer& buffer : uniformBuffers) {
                buffers.push_back(buffer.handle());
            }
            skyPass.create(vulkanDevice.physical(), device, commandPool,
                           vulkanDevice.graphicsQueue(), HdrBuffer::Format, depthBuffer.format(), msaa.sampleCount(), buffers,
                           sizeof(UniformBufferObject), assetManager,
                           vulkanDevice.allocator(),
                           antialiasingLevel == AntialiasingLevel::TAA
                               ? VK_FORMAT_R16G16_SFLOAT
                               : VK_FORMAT_UNDEFINED);
            skyPass.setEnvironment(imageBasedLighting.environmentDescriptor());
        }

        void createSceneSkyPass() {
            std::vector<VkBuffer> buffers;
            buffers.reserve(sceneUniformBuffers.size());
            for (const Buffer& buffer : sceneUniformBuffers) buffers.push_back(buffer.handle());
            sceneSkyPass.create(vulkanDevice.physical(), device, commandPool,
                                vulkanDevice.graphicsQueue(), HdrBuffer::Format, depthBuffer.format(), msaa.sampleCount(), buffers,
                                sizeof(UniformBufferObject), assetManager,
                                vulkanDevice.allocator());
            sceneSkyPass.setEnvironment(imageBasedLighting.environmentDescriptor());
        }

        void createTonemapPass() const {
            tonemapPass.create(device, swapchain.format(), swapchain.extent(),
                               swapchain.imageViews(), hdrBuffer.imageView(),
                               hdrBuffer.sampler(), bloomPass.resultView(), assetManager, temporalAaPass.historyViews());
        }

        void createBloomPass() {
            bloomPass.create(vulkanDevice.physical(), device, swapchain.extent(), vulkanDevice.allocator(), assetManager);
        }

        void createTemporalAaPass() {
            if (antialiasingLevel != AntialiasingLevel::TAA) return;
            temporalAaPass.create(vulkanDevice.physical(), device, swapchain.extent(),
                                  vulkanDevice.allocator(), hdrBuffer.imageView(),
                                  hdrBuffer.sampler(), velocityBuffer.imageView(),
                                  velocityBuffer.sampler(),
                                  (msaa.enabled() ? hiZDepthBuffer : depthBuffer).imageView(),
                                  (msaa.enabled() ? hiZDepthBuffer : depthBuffer).sampler(),
                                  virtualWaterRenderer.velocityView(), virtualWaterRenderer.velocitySampler(),
                                  virtualWaterRenderer.metaView(), virtualWaterRenderer.metaSampler(),
                                  virtualWaterRenderer.surfaceView(), virtualWaterRenderer.surfaceSampler(),
                                  assetManager);
        }

        void createUIResources() {
            const VkExtent2D extent = swapchain.extent();
            scene.uiCanvas().resize(UI::Canvas::Width{extent.width},
                                    UI::Canvas::Height{extent.height});

            if (!fpsFontTexture.valid()) {
                // Keep the font atlas on the same asynchronous upload path as
                // scene textures.  In particular, UI initialization must not
                // fall back to a queue-wide idle while a scene is loading.
                auto uploadBatch = uploadContext.beginBatch();
                const auto& atlas = scene.uiFontAtlas();
                fpsFontTexture.create(vulkanDevice.physical(), device, commandPool,
                                      vulkanDevice.graphicsQueue(), atlas.width(),
                                      atlas.height(), atlas.pixels(), TextureColorSpace::Linear,
                                      false, vulkanDevice.allocator(), TexturePixelFormat::R8);
                [[maybe_unused]] const UploadTicket ticket = uploadBatch.submit();
            }

            canvasRenderer.create(
                vulkanDevice.physical(), device, swapchain.format(), extent,
                swapchain.imageViews(), MAX_FRAMES_IN_FLIGHT, assetManager,
                fpsFontTexture.imageView(), fpsFontTexture.sampler(),
                vulkanDevice.allocator());
        }

        void createEditorUiResources(const bool addViewportTextures = true) {
            if (!editorUiBackend) return;
            const EditorUiInitInfo info{
                reinterpret_cast<std::uint64_t>(instance),
                reinterpret_cast<std::uint64_t>(vulkanDevice.physical()),
                reinterpret_cast<std::uint64_t>(device),
                reinterpret_cast<std::uint64_t>(vulkanDevice.graphicsQueue()),
                vulkanDevice.graphicsQueueFamily(),
                static_cast<std::uint32_t>(swapchain.imageCount()),
                static_cast<std::uint32_t>(swapchain.format())};
            if (!editorUiBackend->initialize(window, info))
                throw std::runtime_error("Could not initialize editor UI backend");
            if (!addViewportTextures) {
                editorUiActive = true;
                return;
            }
            gameViewportDescriptor = reinterpret_cast<VkDescriptorSet>(editorUiBackend->addTexture(
                reinterpret_cast<std::uint64_t>(hdrBuffer.imageView()), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
            if (antialiasingLevel == AntialiasingLevel::TAA) {
                const auto historyViews = temporalAaPass.historyViews();
                for (std::size_t index = 0; index < historyViews.size(); ++index) {
                    gameViewportTemporalDescriptors[index] = reinterpret_cast<VkDescriptorSet>(editorUiBackend->addTexture(
                        reinterpret_cast<std::uint64_t>(historyViews[index]), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
                }
            }
            // The image starts in UNDEFINED layout. Publish it only after a
            // render has transitioned it to SHADER_READ_ONLY_OPTIMAL.
            sceneViewportDescriptor = VK_NULL_HANDLE;
            editorUiActive = true;
        }

        void destroyEditorUiResources() noexcept {
            if (editorUiActive) {
                if (gameViewportDescriptor != VK_NULL_HANDLE) { editorUiBackend->removeTexture(reinterpret_cast<std::uintptr_t>(gameViewportDescriptor));
}
                for (const VkDescriptorSet descriptor : gameViewportTemporalDescriptors) {
                    if (descriptor != VK_NULL_HANDLE) editorUiBackend->removeTexture(reinterpret_cast<std::uintptr_t>(descriptor));
                }
                if (sceneViewportDescriptor != VK_NULL_HANDLE) { editorUiBackend->removeTexture(reinterpret_cast<std::uintptr_t>(sceneViewportDescriptor));
}
            }
            if (editorUiBackend) editorUiBackend->shutdown();
            gameViewportDescriptor = sceneViewportDescriptor = VK_NULL_HANDLE;
            gameViewportTemporalDescriptors.fill(VK_NULL_HANDLE);
            editorUiActive = false;
        }

        // Scene reload replaces the image displayed by the editor. Rebind
        // its descriptors before the next UI command buffer is recorded.
        void refreshEditorViewportTextures() {
            if (!editorUiActive) { return;
}
            if (gameViewportDescriptor != VK_NULL_HANDLE) {
                editorUiBackend->removeTexture(reinterpret_cast<std::uintptr_t>(gameViewportDescriptor));
            }
            for (const VkDescriptorSet descriptor : gameViewportTemporalDescriptors) {
                if (descriptor != VK_NULL_HANDLE) editorUiBackend->removeTexture(reinterpret_cast<std::uintptr_t>(descriptor));
            }
            gameViewportTemporalDescriptors.fill(VK_NULL_HANDLE);
            if (sceneViewportDescriptor != VK_NULL_HANDLE) {
                editorUiBackend->removeTexture(reinterpret_cast<std::uintptr_t>(sceneViewportDescriptor));
            }
            gameViewportDescriptor = reinterpret_cast<VkDescriptorSet>(editorUiBackend->addTexture(
                reinterpret_cast<std::uint64_t>(hdrBuffer.imageView()), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
            if (antialiasingLevel == AntialiasingLevel::TAA) {
                const auto historyViews = temporalAaPass.historyViews();
                for (std::size_t index = 0; index < historyViews.size(); ++index) {
                    gameViewportTemporalDescriptors[index] = reinterpret_cast<VkDescriptorSet>(editorUiBackend->addTexture(
                        reinterpret_cast<std::uint64_t>(historyViews[index]), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
                }
            }
            sceneViewportDescriptor = VK_NULL_HANDLE;
        }
