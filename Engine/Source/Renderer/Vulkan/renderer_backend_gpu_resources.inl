        void createCullingResources() {
            constexpr std::size_t modelDiagonalStride = 5;
            constexpr std::size_t cullingDescriptorBindingCount = 5;
            const std::uint32_t grassBinCount = std::max(1u, static_cast<std::uint32_t>(
                sceneGpu.database.meshes().size() * 3u));

            hiZValid = false;
            const auto objectCount = static_cast<uint32_t>(instanceBatches.size());
            if (objectCount == 0) { return;
}

            hiZBuffer.create(vulkanDevice.physical(), device, swapchain.extent().width, swapchain.extent().height,
                             vulkanDevice.allocator());

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
            const VkDescriptorSetLayoutBinding instanceCullBindings[] = {
                {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            };
            layoutInfo.bindingCount = std::size(instanceCullBindings);
            layoutInfo.pBindings = instanceCullBindings;
            if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr,
                                             &instanceCullingDescriptorSetLayout) != VK_SUCCESS) {
                throw std::runtime_error("Could not create instance-culling descriptor-set layout");
            }
            const VkDescriptorSetLayoutBinding grassBuildBindings[] = {
                {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
            layoutInfo.bindingCount = std::size(grassBuildBindings); layoutInfo.pBindings = grassBuildBindings;
            if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &grassBuildDescriptorSetLayout) != VK_SUCCESS) {
                throw std::runtime_error("Could not create grass-build descriptor-set layout");
            }
            const VkDescriptorSetLayoutBinding grassPrefixBindings[] = {
                {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
            layoutInfo.bindingCount = std::size(grassPrefixBindings); layoutInfo.pBindings = grassPrefixBindings;
            if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &grassPrefixDescriptorSetLayout) != VK_SUCCESS) {
                throw std::runtime_error("Could not create grass-prefix descriptor-set layout");
            }
            const VkDescriptorSetLayoutBinding grassScatterBindings[] = {
                {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
            layoutInfo.bindingCount = std::size(grassScatterBindings); layoutInfo.pBindings = grassScatterBindings;
            if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &grassScatterDescriptorSetLayout) != VK_SUCCESS) {
                throw std::runtime_error("Could not create grass-scatter descriptor-set layout");
            }
            const VkDescriptorSetLayoutBinding grassFinalizeBindings[] = {
                {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
            layoutInfo.bindingCount = std::size(grassFinalizeBindings); layoutInfo.pBindings = grassFinalizeBindings;
            if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &grassFinalizeDescriptorSetLayout) != VK_SUCCESS) {
                throw std::runtime_error("Could not create grass-finalize descriptor-set layout");
            }
            const auto createLayout = [&](VkDescriptorSetLayout setLayout, VkPipelineLayout& pipelineLayout,
                                          const VkPushConstantRange* pushConstantRange = nullptr) {
                VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
                info.setLayoutCount = 1; info.pSetLayouts = &setLayout;
                if (pushConstantRange != nullptr) {
                    info.pushConstantRangeCount = 1;
                    info.pPushConstantRanges = pushConstantRange;
                }
                if (vkCreatePipelineLayout(device, &info, nullptr, &pipelineLayout) != VK_SUCCESS) {
                    throw std::runtime_error("Could not create Hi-Z pipeline layout");
                }
            };
            createLayout(hiZCopyDescriptorSetLayout, hiZCopyPipelineLayout);
            createLayout(hiZReduceDescriptorSetLayout, hiZReducePipelineLayout);
            // mat4 plus draw-slot index, rounded to a 16-byte block by Slang.
            const VkPushConstantRange cullingPushConstants{
                VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Mat4) + 16};
            createLayout(cullingDescriptorSetLayout, cullingPipelineLayout,
                         &cullingPushConstants);
            createLayout(instanceCullingDescriptorSetLayout, instanceCullingPipelineLayout);
            createLayout(grassBuildDescriptorSetLayout, grassBuildPipelineLayout);
            createLayout(grassPrefixDescriptorSetLayout, grassPrefixPipelineLayout);
            createLayout(grassScatterDescriptorSetLayout, grassScatterPipelineLayout);
            createLayout(grassFinalizeDescriptorSetLayout, grassFinalizePipelineLayout);
            hiZCopyPipeline = createComputePipeline("shaders/hiz_initialize.spv", hiZCopyPipelineLayout);
            hiZReducePipeline = createComputePipeline("shaders/hiz_reduce.spv", hiZReducePipelineLayout);
            cullingPipeline = createComputePipeline("shaders/gpu_culling.spv", cullingPipelineLayout);
            instanceCullingPipeline = createComputePipeline("shaders/gpu_instance_culling.spv",
                                                             instanceCullingPipelineLayout);
            grassBuildPipeline = createComputePipeline("shaders/grass_build_indirect.spv", grassBuildPipelineLayout);
            grassPrefixPipeline = createComputePipeline("shaders/grass_prefix_sum.spv", grassPrefixPipelineLayout);
            grassScatterPipeline = createComputePipeline("shaders/grass_scatter_instances.spv", grassScatterPipelineLayout);
            grassFinalizePipeline = createComputePipeline("shaders/grass_finalize_indirect.spv", grassFinalizePipelineLayout);

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
                object.twoSided = batch.twoSided ? 1U : 0U;
                object.lod1IndexCount = batch.lod1IndexCount;
                object.lod2IndexCount = batch.lod2IndexCount;
                object.lod1Distance = batch.lod1Distance;
                object.lod2Distance = batch.lod2Distance;
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
                foliageCullingUniformBuffers[frame].createHostVisible(vulkanDevice.physical(), device,
                    sizeof(Culling::CullingUniformData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                    vulkanDevice.allocator());
                sceneCullingUniformBuffers[frame].createHostVisible(vulkanDevice.physical(), device,
                    sizeof(Culling::CullingUniformData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                    vulkanDevice.allocator());
                sceneFoliageCullingUniformBuffers[frame].createHostVisible(vulkanDevice.physical(), device,
                    sizeof(Culling::CullingUniformData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                    vulkanDevice.allocator());
                shadowCullingUniformBuffers[frame].createHostVisible(vulkanDevice.physical(), device,
                    sizeof(Culling::CullingUniformData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                    vulkanDevice.allocator());
                const std::uint32_t zero = 0;
                std::vector<std::uint32_t> emptyVisibleInstances(
                    std::max<std::size_t>(1, sceneGpu.database.instances().size()));
                visibleInstanceBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device,
                    emptyVisibleInstances.data(), sizeof(std::uint32_t) * emptyVisibleInstances.size(),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                visibleInstanceCountBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device, &zero,
                    sizeof(zero), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                std::vector<std::uint32_t> emptyGrassBins(grassBinCount, 0U);
                grassBinCountBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device,
                    emptyGrassBins.data(), sizeof(std::uint32_t) * emptyGrassBins.size(),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                grassBinOffsetBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device,
                    emptyGrassBins.data(), sizeof(std::uint32_t) * emptyGrassBins.size(),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                grassBinCursorBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device,
                    emptyGrassBins.data(), sizeof(std::uint32_t) * emptyGrassBins.size(),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                const std::vector<GPUGrassInstance> emptyGeneratedGrass =
                    sceneGpu.grassInstances.empty() ? std::vector<GPUGrassInstance>(1)
                                                    : sceneGpu.grassInstances;
                generatedGrassInstanceBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device,
                    emptyGeneratedGrass.data(), sizeof(GPUGrassInstance) * emptyGeneratedGrass.size(),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                const std::vector<GPUGrassCluster> emptyGrassClusters =
                    sceneGpu.grassClusters.empty() ? std::vector<GPUGrassCluster>(1)
                                                   : sceneGpu.grassClusters;
                grassClusterBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device,
                    emptyGrassClusters.data(), sizeof(GPUGrassCluster) * emptyGrassClusters.size(),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                std::vector<VkDrawIndexedIndirectCommand> emptyGrassCommands(grassBinCount);
                grassIndirectBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device,
                    emptyGrassCommands.data(), sizeof(VkDrawIndexedIndirectCommand) * emptyGrassCommands.size(),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool, vulkanDevice.graphicsQueue(),
                    vulkanDevice.allocator());
                grassDrawCountBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device, &zero,
                    sizeof(zero), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool, vulkanDevice.graphicsQueue(),
                    vulkanDevice.allocator());
                grassIndirectUniformBuffers[frame].createHostVisible(vulkanDevice.physical(), device,
                    sizeof(GrassIndirectUniformData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                    vulkanDevice.allocator());
                grassPrefixUniformBuffers[frame].createHostVisible(vulkanDevice.physical(), device,
                    sizeof(GrassPrefixUniformData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                    vulkanDevice.allocator());
                std::vector<VkDrawIndexedIndirectCommand> emptyCommands(objectCount);
                indirectBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device, emptyCommands.data(),
                    sizeof(VkDrawIndexedIndirectCommand) * objectCount,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                auto& lists = grassRenderLists[frame];
                const std::size_t grassCapacity = std::max<std::size_t>(1, sceneGpu.grassInstances.size());
                std::vector<std::uint32_t> emptyVisibleGrass(grassCapacity, 0U);
                std::vector<VkDrawIndexedIndirectCommand> emptyGrassList(std::max<std::size_t>(1, sceneGpu.grassClusters.size()));
                lists.visibleInstances.createDeviceLocal(vulkanDevice.physical(), device, emptyVisibleGrass.data(), sizeof(std::uint32_t) * grassCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                for (Buffer* buffer : {&lists.mainIndirect, &lists.shadowIndirect, &lists.velocityIndirect})
                    buffer->createDeviceLocal(vulkanDevice.physical(), device, emptyGrassList.data(), sizeof(VkDrawIndexedIndirectCommand) * emptyGrassList.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                for (Buffer* buffer : {&lists.mainDrawCount, &lists.shadowDrawCount, &lists.velocityDrawCount})
                    buffer->createDeviceLocal(vulkanDevice.physical(), device, &zero, sizeof(zero), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                foliageIndirectBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device, emptyCommands.data(),
                    sizeof(VkDrawIndexedIndirectCommand) * objectCount,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                sceneIndirectBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device, emptyCommands.data(),
                    sizeof(VkDrawIndexedIndirectCommand) * objectCount,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                sceneFoliageIndirectBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device, emptyCommands.data(),
                    sizeof(VkDrawIndexedIndirectCommand) * objectCount,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                std::vector<VkDrawIndexedIndirectCommand> emptyShadowCommands(
                    objectCount * ShadowMap::MaxPageUpdatesPerFrame);
                shadowIndirectBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device, emptyShadowCommands.data(),
                    sizeof(VkDrawIndexedIndirectCommand) * emptyShadowCommands.size(),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                drawCountBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device, &zero, sizeof(zero),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                foliageDrawCountBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device, &zero, sizeof(zero),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                sceneDrawCountBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device, &zero, sizeof(zero),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                sceneFoliageDrawCountBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device, &zero, sizeof(zero),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                std::array<std::uint32_t, ShadowMap::MaxPageUpdatesPerFrame> zeroShadowCounts{};
                shadowDrawCountBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device,
                    zeroShadowCounts.data(), sizeof(zeroShadowCounts),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
            }

            constexpr uint32_t cullingSetCount = MAX_FRAMES_IN_FLIGHT * 5;
            constexpr uint32_t instanceCullSetCount = MAX_FRAMES_IN_FLIGHT;
            constexpr uint32_t grassSetCount = MAX_FRAMES_IN_FLIGHT * 4;
            const uint32_t imageDescriptors = hiZBuffer.mipCount() + cullingSetCount;
            const VkDescriptorPoolSize poolSizes[] = {
                {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, imageDescriptors},
                {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, hiZBuffer.mipCount()},
                {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cullingSetCount * 3 + instanceCullSetCount * 3 + MAX_FRAMES_IN_FLIGHT * 18},
                {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, cullingSetCount + MAX_FRAMES_IN_FLIGHT * 4},
            };
            VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            poolInfo.maxSets = hiZBuffer.mipCount() + cullingSetCount + instanceCullSetCount + grassSetCount;
            poolInfo.poolSizeCount = std::size(poolSizes); poolInfo.pPoolSizes = poolSizes;
            if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &cullingDescriptorPool) != VK_SUCCESS) {
                throw std::runtime_error("Could not create Hi-Z descriptor pool");
            }
            hiZPass.create(device, cullingDescriptorPool, hiZCopyPipeline, hiZCopyPipelineLayout,
                hiZCopyDescriptorSetLayout, hiZReducePipeline, hiZReducePipelineLayout,
                hiZReduceDescriptorSetLayout, hiZBuffer,
                msaa.enabled() ? hiZDepthBuffer.imageView() : depthBuffer.imageView(),
                msaa.enabled() ? hiZDepthBuffer.sampler() : depthBuffer.sampler());

            std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT * 5> cullLayouts{};
            cullLayouts.fill(cullingDescriptorSetLayout);
            std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT * 5> cullSets{};
            VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            allocateInfo.descriptorPool = cullingDescriptorPool;
            allocateInfo.descriptorSetCount = static_cast<uint32_t>(cullSets.size());
            allocateInfo.pSetLayouts = cullLayouts.data();
            if (vkAllocateDescriptorSets(device, &allocateInfo, cullSets.data()) != VK_SUCCESS) {
                throw std::runtime_error("Could not allocate culling descriptor sets");
            }
            std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> instanceCullLayouts{};
            instanceCullLayouts.fill(instanceCullingDescriptorSetLayout);
            std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> instanceCullSets{};
            allocateInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
            allocateInfo.pSetLayouts = instanceCullLayouts.data();
            if (vkAllocateDescriptorSets(device, &allocateInfo, instanceCullSets.data()) != VK_SUCCESS) {
                throw std::runtime_error("Could not allocate instance-culling descriptor sets");
            }
            std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> grassLayouts{};
            const auto allocateGrassSets = [&](VkDescriptorSetLayout layout,
                                               std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT>& sets) {
                grassLayouts.fill(layout);
                allocateInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
                allocateInfo.pSetLayouts = grassLayouts.data();
                if (vkAllocateDescriptorSets(device, &allocateInfo, sets.data()) != VK_SUCCESS) {
                    throw std::runtime_error("Could not allocate grass compute descriptor sets");
                }
            };
            allocateGrassSets(grassBuildDescriptorSetLayout, grassBuildSets);
            allocateGrassSets(grassPrefixDescriptorSetLayout, grassPrefixSets);
            allocateGrassSets(grassScatterDescriptorSetLayout, grassScatterSets);
            allocateGrassSets(grassFinalizeDescriptorSetLayout, grassFinalizeSets);
            for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
                const VkDescriptorBufferInfo sceneInstanceInfo{
                    gpuSceneInstanceBuffers[frame].handle(), 0, VK_WHOLE_SIZE};
                const VkDescriptorBufferInfo visibleInfo{visibleInstanceBuffers[frame].handle(), 0, VK_WHOLE_SIZE};
                const VkDescriptorBufferInfo visibleCountInfo{visibleInstanceCountBuffers[frame].handle(), 0, VK_WHOLE_SIZE};
                const VkDescriptorBufferInfo instanceUniformInfo{
                    cullingUniformBuffers[frame].handle(), 0, sizeof(Culling::CullingUniformData)};
                std::array<VkWriteDescriptorSet, 4> instanceWrites{};
                const std::array<const VkDescriptorBufferInfo*, 4> instanceInfos{
                    &sceneInstanceInfo, &visibleInfo, &visibleCountInfo, &instanceUniformInfo};
                for (std::uint32_t binding = 0; binding < instanceWrites.size(); ++binding) {
                    instanceWrites[binding] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .dstSet = instanceCullSets[frame], .dstBinding = binding, .descriptorCount = 1,
                        .descriptorType = binding == 3 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                        .pBufferInfo = instanceInfos[binding]};
                }
                vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(instanceWrites.size()),
                                       instanceWrites.data(), 0, nullptr);
                const auto updateGrassSet = [&](const VkDescriptorSet set,
                                                std::initializer_list<VkBuffer> storageBuffers,
                                                const VkBuffer uniformBuffer) {
                    std::vector<VkDescriptorBufferInfo> infos;
                    infos.reserve(storageBuffers.size() + 1);
                    for (const VkBuffer buffer : storageBuffers) infos.push_back({buffer, 0, VK_WHOLE_SIZE});
                    infos.push_back({uniformBuffer, 0, VK_WHOLE_SIZE});
                    std::vector<VkWriteDescriptorSet> writes(infos.size());
                    for (std::uint32_t binding = 0; binding < writes.size(); ++binding) {
                        writes[binding] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                            .dstSet = set, .dstBinding = binding, .descriptorCount = 1,
                            .descriptorType = binding + 1 == writes.size()
                                ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            .pBufferInfo = &infos[binding]};
                    }
                    vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()),
                                           writes.data(), 0, nullptr);
                };
                updateGrassSet(grassBuildSets[frame], {
                    gpuSceneInstanceBuffers[frame].handle(), visibleInstanceBuffers[frame].handle(),
                    visibleInstanceCountBuffers[frame].handle(), gpuSceneMeshBuffers[frame].handle(),
                    grassBinCountBuffers[frame].handle()}, grassIndirectUniformBuffers[frame].handle());
                updateGrassSet(grassPrefixSets[frame], {grassBinCountBuffers[frame].handle(),
                    grassBinOffsetBuffers[frame].handle()}, grassPrefixUniformBuffers[frame].handle());
                updateGrassSet(grassScatterSets[frame], {
                    gpuSceneInstanceBuffers[frame].handle(), visibleInstanceBuffers[frame].handle(),
                    visibleInstanceCountBuffers[frame].handle(), grassBinOffsetBuffers[frame].handle(),
                    grassBinCursorBuffers[frame].handle(), compactGrassInstanceBuffers[frame].handle()},
                    grassIndirectUniformBuffers[frame].handle());
                updateGrassSet(grassFinalizeSets[frame], {gpuSceneMeshBuffers[frame].handle(),
                    grassBinCountBuffers[frame].handle(), grassBinOffsetBuffers[frame].handle(),
                    grassIndirectBuffers[frame].handle(), grassDrawCountBuffers[frame].handle()},
                    grassIndirectUniformBuffers[frame].handle());
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
                    const VkDescriptorBufferInfo countInfo{update.countBuffer.handle(), 0, VK_WHOLE_SIZE};
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
                VkDescriptorSet foliageCullSet = cullSets[MAX_FRAMES_IN_FLIGHT + frame];
                VkDescriptorSet sceneCullSet = cullSets[MAX_FRAMES_IN_FLIGHT * 2 + frame];
                VkDescriptorSet sceneFoliageCullSet = cullSets[MAX_FRAMES_IN_FLIGHT * 3 + frame];
                VkDescriptorSet shadowCullSet = cullSets[MAX_FRAMES_IN_FLIGHT * 4 + frame];
                updateCullingSet({cameraCullSet, indirectBuffers[frame], drawCountBuffers[frame],
                                  cullingUniformBuffers[frame]});
                updateCullingSet({foliageCullSet, foliageIndirectBuffers[frame], foliageDrawCountBuffers[frame],
                                  foliageCullingUniformBuffers[frame]});
                updateCullingSet({shadowCullSet, shadowIndirectBuffers[frame], shadowDrawCountBuffers[frame],
                                  shadowCullingUniformBuffers[frame]});
                updateCullingSet({sceneCullSet, sceneIndirectBuffers[frame], sceneDrawCountBuffers[frame],
                                  sceneCullingUniformBuffers[frame]});
                updateCullingSet({sceneFoliageCullSet, sceneFoliageIndirectBuffers[frame], sceneFoliageDrawCountBuffers[frame],
                                  sceneFoliageCullingUniformBuffers[frame]});
                instanceCullingPasses[frame].create(instanceCullingPipeline, instanceCullingPipelineLayout,
                    instanceCullSets[frame], visibleInstanceCountBuffers[frame].handle(),
                    visibleInstanceBuffers[frame].handle());
                gpuCullingPasses[frame].create(device, cullingPipeline, cullingPipelineLayout, cullSets[frame],
                    indirectBuffers[frame].handle(), drawCountBuffers[frame].handle(), objectCount);
                indirectDraws[frame].create(
                    indirectBuffers[frame].handle(), drawCountBuffers[frame].handle(), objectCount);
                foliageGpuCullingPasses[frame].create(device, cullingPipeline, cullingPipelineLayout, foliageCullSet,
                    foliageIndirectBuffers[frame].handle(), foliageDrawCountBuffers[frame].handle(), objectCount);
                // Grass is culled as terrain chunks in the production draw
                // path.  The experimental GPU-scene per-instance list remains
                // available for the compaction passes, but must not replace a
                // valid chunk command list until its bounds source is shared
                // with terrain extraction.
                foliageIndirectDraws[frame].create(
                    foliageIndirectBuffers[frame].handle(), foliageDrawCountBuffers[frame].handle(), objectCount);
                sceneGpuCullingPasses[frame].create(device, cullingPipeline, cullingPipelineLayout, sceneCullSet,
                    sceneIndirectBuffers[frame].handle(), sceneDrawCountBuffers[frame].handle(), objectCount);
                sceneIndirectDraws[frame].create(
                    sceneIndirectBuffers[frame].handle(), sceneDrawCountBuffers[frame].handle(), objectCount);
                sceneFoliageGpuCullingPasses[frame].create(device, cullingPipeline, cullingPipelineLayout, sceneFoliageCullSet,
                    sceneFoliageIndirectBuffers[frame].handle(), sceneFoliageDrawCountBuffers[frame].handle(), objectCount);
                sceneFoliageIndirectDraws[frame].create(
                    sceneFoliageIndirectBuffers[frame].handle(), sceneFoliageDrawCountBuffers[frame].handle(), objectCount);
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
            for (auto& draw : foliageIndirectDraws) draw.destroy();
            for (auto& draw : sceneIndirectDraws) draw.destroy();
            for (auto& draw : sceneFoliageIndirectDraws) draw.destroy();
            for (auto& draw : shadowIndirectDraws) draw.destroy();
            for (Buffer& buffer : cullingUniformBuffers) buffer.destroy();
            for (Buffer& buffer : foliageCullingUniformBuffers) buffer.destroy();
            for (Buffer& buffer : sceneCullingUniformBuffers) buffer.destroy();
            for (Buffer& buffer : sceneFoliageCullingUniformBuffers) buffer.destroy();
            for (Buffer& buffer : shadowCullingUniformBuffers) buffer.destroy();
            for (Buffer& buffer : indirectBuffers) buffer.destroy();
            for (Buffer& buffer : foliageIndirectBuffers) buffer.destroy();
            for (Buffer& buffer : sceneIndirectBuffers) buffer.destroy();
            for (Buffer& buffer : sceneFoliageIndirectBuffers) buffer.destroy();
            for (Buffer& buffer : shadowIndirectBuffers) buffer.destroy();
            for (Buffer& buffer : drawCountBuffers) buffer.destroy();
            for (Buffer& buffer : foliageDrawCountBuffers) buffer.destroy();
            for (Buffer& buffer : sceneDrawCountBuffers) buffer.destroy();
            for (Buffer& buffer : sceneFoliageDrawCountBuffers) buffer.destroy();
            for (Buffer& buffer : shadowDrawCountBuffers) buffer.destroy();
            for (Buffer& buffer : cullingObjectBuffers) buffer.destroy();
            for (Buffer& buffer : visibleInstanceBuffers) buffer.destroy();
            for (Buffer& buffer : visibleInstanceCountBuffers) buffer.destroy();
            for (Buffer& buffer : grassBinCountBuffers) buffer.destroy();
            for (Buffer& buffer : grassBinOffsetBuffers) buffer.destroy();
            for (Buffer& buffer : grassBinCursorBuffers) buffer.destroy();
            for (Buffer& buffer : generatedGrassInstanceBuffers) buffer.destroy();
            for (Buffer& buffer : grassIndirectBuffers) buffer.destroy();
            for (Buffer& buffer : grassDrawCountBuffers) buffer.destroy();
            for (Buffer& buffer : grassIndirectUniformBuffers) buffer.destroy();
            for (Buffer& buffer : grassPrefixUniformBuffers) buffer.destroy();
            if (cullingDescriptorPool != VK_NULL_HANDLE) { vkDestroyDescriptorPool(device, cullingDescriptorPool, nullptr);
}
            if (hiZCopyPipeline != VK_NULL_HANDLE) { vkDestroyPipeline(device, hiZCopyPipeline, nullptr);
}
            if (hiZReducePipeline != VK_NULL_HANDLE) { vkDestroyPipeline(device, hiZReducePipeline, nullptr);
}
            if (cullingPipeline != VK_NULL_HANDLE) { vkDestroyPipeline(device, cullingPipeline, nullptr);
}
            if (instanceCullingPipeline != VK_NULL_HANDLE) { vkDestroyPipeline(device, instanceCullingPipeline, nullptr);
}
            if (grassBuildPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, grassBuildPipeline, nullptr);
            if (grassPrefixPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, grassPrefixPipeline, nullptr);
            if (grassScatterPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, grassScatterPipeline, nullptr);
            if (grassFinalizePipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, grassFinalizePipeline, nullptr);
            if (hiZCopyPipelineLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, hiZCopyPipelineLayout, nullptr);
}
            if (hiZReducePipelineLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, hiZReducePipelineLayout, nullptr);
}
            if (cullingPipelineLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, cullingPipelineLayout, nullptr);
}
            if (instanceCullingPipelineLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, instanceCullingPipelineLayout, nullptr);
}
            if (grassBuildPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, grassBuildPipelineLayout, nullptr);
            if (grassPrefixPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, grassPrefixPipelineLayout, nullptr);
            if (grassScatterPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, grassScatterPipelineLayout, nullptr);
            if (grassFinalizePipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, grassFinalizePipelineLayout, nullptr);
            if (hiZCopyDescriptorSetLayout != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device, hiZCopyDescriptorSetLayout, nullptr);
}
            if (hiZReduceDescriptorSetLayout != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device, hiZReduceDescriptorSetLayout, nullptr);
}
            if (cullingDescriptorSetLayout != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device, cullingDescriptorSetLayout, nullptr);
}
            if (instanceCullingDescriptorSetLayout != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device, instanceCullingDescriptorSetLayout, nullptr);
}
            if (grassBuildDescriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, grassBuildDescriptorSetLayout, nullptr);
            if (grassPrefixDescriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, grassPrefixDescriptorSetLayout, nullptr);
            if (grassScatterDescriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, grassScatterDescriptorSetLayout, nullptr);
            if (grassFinalizeDescriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, grassFinalizeDescriptorSetLayout, nullptr);
            cullingDescriptorPool = VK_NULL_HANDLE; hiZCopyPipeline = hiZReducePipeline = cullingPipeline = VK_NULL_HANDLE;
            instanceCullingPipeline = VK_NULL_HANDLE;
            grassBuildPipeline = grassPrefixPipeline = grassScatterPipeline = grassFinalizePipeline = VK_NULL_HANDLE;
            hiZCopyPipelineLayout = hiZReducePipelineLayout = cullingPipelineLayout = VK_NULL_HANDLE;
            instanceCullingPipelineLayout = VK_NULL_HANDLE;
            grassBuildPipelineLayout = grassPrefixPipelineLayout = grassScatterPipelineLayout = grassFinalizePipelineLayout = VK_NULL_HANDLE;
            hiZCopyDescriptorSetLayout = hiZReduceDescriptorSetLayout = cullingDescriptorSetLayout = VK_NULL_HANDLE;
            instanceCullingDescriptorSetLayout = VK_NULL_HANDLE;
            grassBuildDescriptorSetLayout = grassPrefixDescriptorSetLayout = grassScatterDescriptorSetLayout = grassFinalizeDescriptorSetLayout = VK_NULL_HANDLE;
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

            if (antialiasingLevel == AntialiasingLevel::TAA) {
                velocityBuffer.create(vulkanDevice.physical(), device, extent,
                                      vulkanDevice.allocator(), VK_FILTER_NEAREST);
                GraphicsPipelineOptions velocityOptions{};
                velocityOptions.colorFormat = HdrBuffer::Format;
                velocityOptions.depthFormat = depthBuffer.format();
                velocityOptions.colorFinalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                velocityOptions.depthLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                velocityOptions.depthInitialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                velocityOptions.depthFinalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                velocityOptions.shader = "shaders/temporal_velocity.spv";
                velocityOptions.assetManager = &assetManager;
                velocityOptions.cullMode = VK_CULL_MODE_BACK_BIT;
                velocityOptions.depthTestEnable = VK_TRUE;
                velocityOptions.depthWriteEnable = VK_FALSE;
                velocityOptions.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
                velocityOptions.descriptorSetLayouts = {shadowPass.descriptorSetLayout()};
                velocityOptions.vertexBindings = {
                    {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX}};
                velocityOptions.vertexAttributes = {
                    {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
                    {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, texCoord)},
                    {8, 0, VK_FORMAT_R32_UINT, offsetof(Vertex, materialIndex)},
                    };
                velocityPipeline.create(device, velocityOptions);
                GraphicsPipelineOptions foliageVelocityOptions = velocityOptions;
                foliageVelocityOptions.existingRenderPass = velocityPipeline.renderPass();
                foliageVelocityOptions.cullMode = VK_CULL_MODE_NONE;
                foliageVelocityPipeline.create(device, foliageVelocityOptions);

                const VkImageView velocityAttachments[] = {
                    velocityBuffer.imageView(), depthBuffer.imageView()};
                VkFramebufferCreateInfo velocityInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
                velocityInfo.renderPass = velocityPipeline.renderPass();
                velocityInfo.attachmentCount = 2;
                velocityInfo.pAttachments = velocityAttachments;
                velocityInfo.width = extent.width;
                velocityInfo.height = extent.height;
                velocityInfo.layers = 1;
                if (vkCreateFramebuffer(device, &velocityInfo, nullptr,
                                        &velocityFramebuffer) != VK_SUCCESS) {
                    throw std::runtime_error("Could not create TAA velocity framebuffer");
                }
            }
        }

        void destroyVelocityResources() noexcept {
            if (velocityFramebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device, velocityFramebuffer, nullptr);
                velocityFramebuffer = VK_NULL_HANDLE;
            }
            foliageVelocityPipeline.destroy();
            velocityPipeline.destroy();
            velocityBuffer.destroy();
        }

        void createSceneViewportResources() {
            // Start with the drawable extent. The editor will later supply its
            // panel extent through the renderer viewport API; creating it here
            // also makes the off-screen lifecycle valid for non-editor users.
            sceneViewportTarget.create(vulkanDevice.physical(), device, swapchain.extent(),
                                       msaa.sampleCount(), vulkanDevice.allocator());
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

        std::uint32_t updateVirtualShadowClipmaps(
            const Vec3& cameraPosition,
            std::array<Mat4, ShadowMap::ClipLevelCount>& matrices,
            Vec3& previousCameraPosition, Vec3& previousLightDirection,
            bool& valid) const {
            const DirectionalLight light = directionalLight();
            const float cameraJump = valid
                ? (cameraPosition - previousCameraPosition).length()
                : std::numeric_limits<float>::max();
            const float lightAgreement = valid
                ? dot(light.direction, previousLightDirection)
                : -1.0F;
            const bool invalidate = !valid || cameraJump > 8.0F || lightAgreement < 0.9995F;
            std::uint32_t updateMask = invalidate ? 0xFu : 0x1u;
            if (!invalidate) {
                if ((shadowClipFrameIndex & 1u) == 0) updateMask |= 0x2u;
                if ((shadowClipFrameIndex & 3u) == 0) updateMask |= 0x4u;
                if ((shadowClipFrameIndex & 7u) == 0) updateMask |= 0x8u;
            }

            constexpr std::array<float, 3> baseExtents{12.0F, 36.0F, 108.0F};
            const float cameraSceneDistance = (cameraPosition - sceneCenter).length();
            const float farExtent = std::max(324.0F,
                (cameraSceneDistance + sceneRadius) * 1.1F);
            const Vec3 direction = light.direction.normalized();
            Vec3 up{0.0F, 1.0F, 0.0F};
            if (std::abs(dot(direction, up)) > 0.98F) up = Vec3{0.0F, 0.0F, 1.0F};
            const Vec3 right = cross(direction, up).normalized();
            const Vec3 lightUp = cross(right, direction).normalized();

            for (std::uint32_t level = 0; level < ShadowMap::ClipLevelCount; ++level) {
                if ((updateMask & (1u << level)) == 0) continue;
                const float extent = level < baseExtents.size() ? baseExtents[level] : farExtent;
                // Page-sized snapping makes the virtual address space stable
                // while the camera moves inside a page. Cached physical pages
                // therefore survive ordinary sub-page camera motion.
                const float worldUnitsPerPage = extent * 2.0F /
                    static_cast<float>(ShadowMap::VirtualPagesPerAxis);
                const float snappedX = std::round(dot(cameraPosition, right) / worldUnitsPerPage) *
                                       worldUnitsPerPage;
                const float snappedY = std::round(dot(cameraPosition, lightUp) / worldUnitsPerPage) *
                                       worldUnitsPerPage;
                // Keep depth encoding independent of the camera so pages can
                // be remapped when the clipmap scrolls without being redrawn.
                const float alongLight = dot(sceneCenter, direction);
                const Vec3 target = right * snappedX + lightUp * snappedY + direction * alongLight;
                const float lightDistance = sceneRadius * 2.0F + extent;
                const Mat4 lightView = Mat4::lookAt(target - direction * lightDistance,
                                                    target, lightUp);
                const Mat4 lightProjection = Mat4::scale(
                    Mat4::ortho(-extent, extent, -extent, extent, 0.1F,
                                sceneRadius * 4.0F + extent * 2.0F),
                    Vec3{1.0F, -1.0F, 1.0F});
                matrices[level] = lightProjection * lightView;
            }
            previousCameraPosition = cameraPosition;
            previousLightDirection = light.direction;
            valid = true;
            return updateMask;
        }
