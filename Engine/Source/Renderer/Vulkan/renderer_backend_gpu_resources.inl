        void createCullingResources() {
            constexpr std::size_t modelDiagonalStride = 5;
            constexpr std::size_t cullingDescriptorBindingCount = 5;

            hiZValid = false;
            const auto objectCount = static_cast<uint32_t>(instanceBatches.size());
            if (objectCount == 0) { return;
}

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
                object.model.data[0] = 1.0F;
                object.model.data[modelDiagonalStride] = 1.0F;
                object.model.data[modelDiagonalStride * 2] = 1.0F;
                object.model.data[modelDiagonalStride * 3] = 1.0F;
                const AABB& bounds = batch.worldBounds;
                object.localAabbMin = {
                    bounds.min.x(), bounds.min.y(), bounds.min.z(), 0.0F, };
                object.localAabbMax = {
                    bounds.max.x(), bounds.max.y(), bounds.max.z(), 0.0F, };
                object.indexCount = batch.indexCount;
                object.instanceCount = batch.instanceCount;
                object.firstIndex = batch.firstIndex;
                object.vertexOffset = 0;
                object.firstInstance = batch.firstInstance;
                object.castShadow = batch.castShadow ? 1U : 0U;
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
                sceneCullingUniformBuffers[frame].createHostVisible(vulkanDevice.physical(), device,
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
                sceneIndirectBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device, emptyCommands.data(),
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
                sceneDrawCountBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device, &zero, sizeof(zero),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                shadowDrawCountBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device, &zero, sizeof(zero),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
            }

            constexpr uint32_t cullingSetCount = MAX_FRAMES_IN_FLIGHT * 3;
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

            std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT * 3> cullLayouts{};
            cullLayouts.fill(cullingDescriptorSetLayout);
            std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT * 3> cullSets{};
            VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            allocateInfo.descriptorPool = cullingDescriptorPool;
            allocateInfo.descriptorSetCount = static_cast<uint32_t>(cullSets.size());
            allocateInfo.pSetLayouts = cullLayouts.data();
            if (vkAllocateDescriptorSets(device, &allocateInfo, cullSets.data()) != VK_SUCCESS) {
                throw std::runtime_error("Could not allocate culling descriptor sets");
            }
            for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
                const VkDescriptorImageInfo hiZInfo{hiZBuffer.sampler(), hiZBuffer.fullView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                struct CullingSetUpdate {
                    VkDescriptorSet set;
                    const Buffer& indirectBuffer;
                    const Buffer& countBuffer;
                    const Buffer& uniformBuffer;
                };
                const auto updateCullingSet = [&](const CullingSetUpdate& update) {
                    const VkDescriptorBufferInfo objectInfo{
                        cullingObjectBuffers[frame].handle(), 0, VK_WHOLE_SIZE};
                    const VkDescriptorBufferInfo indirectInfo{update.indirectBuffer.handle(), 0, VK_WHOLE_SIZE};
                    const VkDescriptorBufferInfo countInfo{update.countBuffer.handle(), 0, sizeof(uint32_t)};
                    const VkDescriptorBufferInfo uniformInfo{update.uniformBuffer.handle(), 0, sizeof(Culling::CullingUniformData)};
                    std::array<VkWriteDescriptorSet, cullingDescriptorBindingCount> writes{};
                    for (uint32_t i = 0; i < writes.size(); ++i) {
                        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                        writes[i].dstSet = update.set;
                        writes[i].dstBinding = i;
                        writes[i].descriptorCount = 1;
                    }
                    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[0].pBufferInfo = &objectInfo;
                    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[1].pBufferInfo = &indirectInfo;
                    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[2].pBufferInfo = &countInfo;
                    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[3].pImageInfo = &hiZInfo;
                    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; writes[4].pBufferInfo = &uniformInfo;
                    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
                };
                VkDescriptorSet cameraCullSet = cullSets[frame];
                VkDescriptorSet sceneCullSet = cullSets[MAX_FRAMES_IN_FLIGHT + frame];
                VkDescriptorSet shadowCullSet = cullSets[MAX_FRAMES_IN_FLIGHT * 2 + frame];
                updateCullingSet({cameraCullSet, indirectBuffers[frame], drawCountBuffers[frame],
                                  cullingUniformBuffers[frame]});
                updateCullingSet({shadowCullSet, shadowIndirectBuffers[frame], shadowDrawCountBuffers[frame],
                                  shadowCullingUniformBuffers[frame]});
                updateCullingSet({sceneCullSet, sceneIndirectBuffers[frame], sceneDrawCountBuffers[frame],
                                  sceneCullingUniformBuffers[frame]});
                gpuCullingPasses[frame].create(device, cullingPipeline, cullingPipelineLayout, cullSets[frame],
                    indirectBuffers[frame].handle(), drawCountBuffers[frame].handle(), objectCount);
                indirectDraws[frame].create(
                    indirectBuffers[frame].handle(), drawCountBuffers[frame].handle(), objectCount);
                sceneGpuCullingPasses[frame].create(device, cullingPipeline, cullingPipelineLayout, sceneCullSet,
                    sceneIndirectBuffers[frame].handle(), sceneDrawCountBuffers[frame].handle(), objectCount);
                sceneIndirectDraws[frame].create(
                    sceneIndirectBuffers[frame].handle(), sceneDrawCountBuffers[frame].handle(), objectCount);
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
            for (auto& draw : sceneIndirectDraws) draw.destroy();
            for (auto& draw : shadowIndirectDraws) draw.destroy();
            for (Buffer& buffer : cullingUniformBuffers) buffer.destroy();
            for (Buffer& buffer : sceneCullingUniformBuffers) buffer.destroy();
            for (Buffer& buffer : shadowCullingUniformBuffers) buffer.destroy();
            for (Buffer& buffer : indirectBuffers) buffer.destroy();
            for (Buffer& buffer : sceneIndirectBuffers) buffer.destroy();
            for (Buffer& buffer : shadowIndirectBuffers) buffer.destroy();
            for (Buffer& buffer : drawCountBuffers) buffer.destroy();
            for (Buffer& buffer : sceneDrawCountBuffers) buffer.destroy();
            for (Buffer& buffer : shadowDrawCountBuffers) buffer.destroy();
            for (Buffer& buffer : cullingObjectBuffers) buffer.destroy();
            if (cullingDescriptorPool != VK_NULL_HANDLE) { vkDestroyDescriptorPool(device, cullingDescriptorPool, nullptr);
}
            if (hiZCopyPipeline != VK_NULL_HANDLE) { vkDestroyPipeline(device, hiZCopyPipeline, nullptr);
}
            if (hiZReducePipeline != VK_NULL_HANDLE) { vkDestroyPipeline(device, hiZReducePipeline, nullptr);
}
            if (cullingPipeline != VK_NULL_HANDLE) { vkDestroyPipeline(device, cullingPipeline, nullptr);
}
            if (hiZCopyPipelineLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, hiZCopyPipelineLayout, nullptr);
}
            if (hiZReducePipelineLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, hiZReducePipelineLayout, nullptr);
}
            if (cullingPipelineLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, cullingPipelineLayout, nullptr);
}
            if (hiZCopyDescriptorSetLayout != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device, hiZCopyDescriptorSetLayout, nullptr);
}
            if (hiZReduceDescriptorSetLayout != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device, hiZReduceDescriptorSetLayout, nullptr);
}
            if (cullingDescriptorSetLayout != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device, cullingDescriptorSetLayout, nullptr);
}
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
            createSceneViewportFramebuffer();
        }

        void createSceneViewportFramebuffer() {
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
            destroySceneViewportFramebuffer();
            sceneViewportTarget.destroy();
        }

        void destroySceneViewportFramebuffer() noexcept {
            if (sceneViewportFramebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device, sceneViewportFramebuffer, nullptr);
                sceneViewportFramebuffer = VK_NULL_HANDLE;
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

        [[nodiscard]] Mat4 lightSpaceMatrix() const {
            const auto light = directionalLight();
            const float extent = sceneRadius;
            const Vec3 lightTarget = sceneCenter;
            const Vec3 lightPosition = lightTarget - (light.direction * (extent * 5.0F));
            constexpr Vec3 worldUp{0.0F, 1.0F, 0.0F};
            const Mat4 lightView = Mat4::lookAt(lightPosition, lightTarget, worldUp);
            const Mat4 lightProjection = Mat4::scale(
                Mat4::ortho(-extent * 2.25F, extent * 2.25F,
                            -extent * 2.25F, extent * 2.25F,
                            0.1F, extent * 8.0F),
                Vec3{1.0F, -1.0F, 1.0F});
            return lightProjection * lightView;
        }
