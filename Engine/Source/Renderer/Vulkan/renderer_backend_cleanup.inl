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
                sceneViewportForwardPass.destroy();
                forwardPass.destroy();
                shadowPass.destroy();
                sceneDescriptorPass.destroy();
                // Cubemap owns samplers, views, images and memory; the BRDF
                // LUT is VMA-backed.  Release all IBL resources while both
                // the logical device and allocator still exist.  Otherwise
                // the member Cubemap destructors run after vkDestroyDevice().
                imageBasedLighting.destroy();
                physicalShadowPagePool.destroy();
                destroyCullingResources();
                indexBuffer.destroy();
                vertexBuffer.destroy();
                meshletTriangleBuffer.destroy();
                meshletVertexBuffer.destroy();
                meshletBuffer.destroy();
                for (Buffer& buffer : instanceBuffers) {
                    buffer.destroy();
                }
                for (Buffer& buffer : previousTransformBuffers) {
                    buffer.destroy();
                }
                for (auto& retiredBuffers : deferredPreviousTransformBuffers) {
                    retiredBuffers.clear();
                }
                for (Buffer& buffer : compactGrassInstanceBuffers) {
                    buffer.destroy();
                }
                for (Buffer& buffer : grassClusterBuffers) {
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
                for (Buffer& buffer : visibleMeshletBuffers) buffer.destroy();
                for (Buffer& buffer : visibleMeshletCountBuffers) buffer.destroy();
                for (Buffer& buffer : meshletCullingUniformBuffers) buffer.destroy();
                for (Buffer& uniformBuffer : uniformBuffers) {
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
                grassHeightTexture.destroy();
                grassDensityTexture.destroy();
                uploadContext.destroy();
                gpuTimestampProfiler.destroy();

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

                if (asyncComputeTimeline != VK_NULL_HANDLE) {
                    vkDestroySemaphore(device, asyncComputeTimeline, nullptr);
                    asyncComputeTimeline = VK_NULL_HANDLE;
                }
                asyncComputeCommandBuffers.clear();
                postAsyncGraphicsCommandBuffers.clear();
                if (asyncComputeCommandPool != VK_NULL_HANDLE) {
                    vkDestroyCommandPool(device, asyncComputeCommandPool, nullptr);
                    asyncComputeCommandPool = VK_NULL_HANDLE;
                }

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
