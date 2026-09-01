        void cleanup() {
            if (this->cleanedUp) {
                return;
            }
            this->cleanedUp = true;

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
                for (Buffer& buffer : materialBuffers) {
                    buffer.destroy();
                }
                for (Buffer& buffer : gpuSceneInstanceBuffers) buffer.destroy();
                for (Buffer& buffer : gpuSceneMeshBuffers) buffer.destroy();
                for (Buffer& buffer : gpuSceneMaterialBuffers) buffer.destroy();
                for (Buffer& buffer : visibleInstanceBuffers) buffer.destroy();
                for (Buffer& buffer : visibleInstanceCountBuffers) buffer.destroy();
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
