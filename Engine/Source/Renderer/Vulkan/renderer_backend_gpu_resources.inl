        void createCullingResources() {
            constexpr std::size_t modelDiagonalStride = 5;
            constexpr std::size_t cullingDescriptorBindingCount = 7;
            const std::uint32_t grassBinCount = std::max(1u, static_cast<std::uint32_t>(
                sceneGpu.database.meshes().size() * 3u));

            hiZValid = false;
            const auto objectCount = static_cast<uint32_t>(instanceBatches.size());
            const auto grassInstanceCount = static_cast<uint32_t>(sceneGpu.grassInstances.size());
            if (objectCount == 0 && grassInstanceCount == 0) return;
            // Generic descriptors remain valid in a grass-only scene, but
            // their backing allocations need one inert element. Their draw
            // counts stay zero because objectCount itself remains zero.
            const auto genericCapacity = std::max(1u, objectCount);

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
                {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
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
            const VkDescriptorSetLayoutBinding grassDispatchBuildBindings[] = {
                {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
            layoutInfo.bindingCount = std::size(grassDispatchBuildBindings);
            layoutInfo.pBindings = grassDispatchBuildBindings;
            if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &grassDispatchBuildDescriptorSetLayout) != VK_SUCCESS) {
                throw std::runtime_error("Could not create grass dispatch-build descriptor-set layout");
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
            const VkDescriptorSetLayoutBinding grassPackedCullBindings[] = {
                {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
            layoutInfo.bindingCount = std::size(grassPackedCullBindings); layoutInfo.pBindings = grassPackedCullBindings;
            if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &grassPackedCullDescriptorSetLayout) != VK_SUCCESS) {
                throw std::runtime_error("Could not create packed grass-cull descriptor-set layout");
            }
            const VkDescriptorSetLayoutBinding grassBladeCullBindings[] = {
                {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
            layoutInfo.bindingCount = std::size(grassBladeCullBindings); layoutInfo.pBindings = grassBladeCullBindings;
            if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &grassBladeCullDescriptorSetLayout) != VK_SUCCESS)
                throw std::runtime_error("Could not create grass blade-cull descriptor-set layout");
            const VkDescriptorSetLayoutBinding grassClassifyBindings[] = {
                {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {8, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
            layoutInfo.bindingCount = std::size(grassClassifyBindings); layoutInfo.pBindings = grassClassifyBindings;
            if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &grassClassifyDescriptorSetLayout) != VK_SUCCESS) {
                throw std::runtime_error("Could not create grass-classify descriptor-set layout");
            }
            const VkDescriptorSetLayoutBinding grassPackedBinBindings[] = {
                {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
            layoutInfo.bindingCount = std::size(grassPackedBinBindings); layoutInfo.pBindings = grassPackedBinBindings;
            if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &grassPackedBinDescriptorSetLayout) != VK_SUCCESS) throw std::runtime_error("Could not create packed grass-bin layout");
            const VkDescriptorSetLayoutBinding grassPackedScatterBindings[] = {
                {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
            layoutInfo.bindingCount = std::size(grassPackedScatterBindings); layoutInfo.pBindings = grassPackedScatterBindings;
            if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &grassPackedScatterDescriptorSetLayout) != VK_SUCCESS) throw std::runtime_error("Could not create packed grass-scatter layout");
            const VkDescriptorSetLayoutBinding grassPackedFinalizeBindings[] = {
                {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
            layoutInfo.bindingCount = std::size(grassPackedFinalizeBindings); layoutInfo.pBindings = grassPackedFinalizeBindings;
            if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &grassPackedFinalizeDescriptorSetLayout) != VK_SUCCESS) throw std::runtime_error("Could not create packed grass-finalize layout");
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
            createLayout(grassDispatchBuildDescriptorSetLayout, grassDispatchBuildPipelineLayout);
            createLayout(grassPrefixDescriptorSetLayout, grassPrefixPipelineLayout);
            createLayout(grassScatterDescriptorSetLayout, grassScatterPipelineLayout);
            createLayout(grassFinalizeDescriptorSetLayout, grassFinalizePipelineLayout);
            createLayout(grassPackedCullDescriptorSetLayout, grassPackedCullPipelineLayout);
            createLayout(grassBladeCullDescriptorSetLayout, grassBladeCullPipelineLayout);
            createLayout(grassClassifyDescriptorSetLayout, grassClassifyPipelineLayout);
            createLayout(grassPackedBinDescriptorSetLayout, grassPackedBinPipelineLayout);
            createLayout(grassPackedScatterDescriptorSetLayout, grassPackedScatterPipelineLayout);
            createLayout(grassPackedFinalizeDescriptorSetLayout, grassPackedFinalizePipelineLayout);
            hiZCopyPipeline = createComputePipeline("shaders/hiz_initialize.spv", hiZCopyPipelineLayout);
            hiZReducePipeline = createComputePipeline("shaders/hiz_reduce.spv", hiZReducePipelineLayout);
            cullingPipeline = createComputePipeline("shaders/gpu_culling.spv", cullingPipelineLayout);
            instanceCullingPipeline = createComputePipeline("shaders/gpu_instance_culling.spv",
                                                             instanceCullingPipelineLayout);
            grassBuildPipeline = createComputePipeline("shaders/grass_build_indirect.spv", grassBuildPipelineLayout);
            grassDispatchBuildPipeline = createComputePipeline("shaders/grass_build_dispatch.spv", grassDispatchBuildPipelineLayout);
            grassPrefixPipeline = createComputePipeline("shaders/grass_prefix_sum.spv", grassPrefixPipelineLayout);
            grassScatterPipeline = createComputePipeline("shaders/grass_scatter_instances.spv", grassScatterPipelineLayout);
            grassFinalizePipeline = createComputePipeline("shaders/grass_finalize_indirect.spv", grassFinalizePipelineLayout);
            grassBladeCullPipeline = createComputePipeline("shaders/grass_packed_cull.spv", grassBladeCullPipelineLayout);
            // Cluster cull has the compact 5-binding layout retained above.
            grassPackedCullPipeline = createComputePipeline("shaders/grass_cluster_cull.spv", grassPackedCullPipelineLayout);
            grassClassifyPipeline = createComputePipeline("shaders/grass_classify.spv", grassClassifyPipelineLayout);
            grassPackedBinPipeline = createComputePipeline("shaders/grass_packed_bin.spv", grassPackedBinPipelineLayout);
            grassPackedPrefixPipeline = createComputePipeline("shaders/grass_packed_prefix.spv", grassPrefixPipelineLayout);
            grassPackedScatterPipeline = createComputePipeline("shaders/grass_packed_scatter.spv", grassPackedScatterPipelineLayout);
            grassPackedFinalizePipeline = createComputePipeline("shaders/grass_packed_finalize.spv", grassPackedFinalizePipelineLayout);

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
                    sizeof(Culling::GPUObjectData) * genericCapacity,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vulkanDevice.allocator());
                const Culling::GPUObjectData emptyObject{};
                buffer.update(objectCount == 0 ? &emptyObject : gpuObjects.data(),
                              sizeof(Culling::GPUObjectData) * genericCapacity);
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
                std::vector<VkDrawIndexedIndirectCommand> emptyCommands(genericCapacity);
                indirectBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device, emptyCommands.data(),
                    sizeof(VkDrawIndexedIndirectCommand) * genericCapacity,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                auto& lists = grassRenderLists[frame];
                const std::size_t grassCapacity = std::max<std::size_t>(1, sceneGpu.grassInstances.size());
                std::vector<std::uint32_t> emptyVisibleGrass(grassCapacity, 0U);
                std::vector<VkDrawIndexedIndirectCommand> emptyGrassList(std::max<std::size_t>(1, sceneGpu.grassClusters.size()));
                lists.visibleInstances.createDeviceLocal(vulkanDevice.physical(), device, emptyVisibleGrass.data(), sizeof(std::uint32_t) * grassCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                const std::array<std::uint32_t, 3> zeroDispatchCounts{};
                lists.visibleCount.createDeviceLocal(vulkanDevice.physical(), device, zeroDispatchCounts.data(), sizeof(zeroDispatchCounts), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                const std::size_t clusterCapacity = std::max<std::size_t>(1, sceneGpu.grassClusters.size());
                std::vector<std::uint32_t> emptyVisibleClusters(clusterCapacity, 0U);
                lists.visibleClusters.createDeviceLocal(vulkanDevice.physical(), device, emptyVisibleClusters.data(), sizeof(std::uint32_t) * clusterCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                lists.visibleClusterCount.createDeviceLocal(vulkanDevice.physical(), device, &zero, sizeof(zero), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                const auto maxClusterInstances = std::max_element(sceneGpu.grassClusters.begin(), sceneGpu.grassClusters.end(), [](const auto& a, const auto& b) { return a.instanceRange.y < b.instanceRange.y; });
                const GrassBladeDispatchData bladeDispatch{maxClusterInstances == sceneGpu.grassClusters.end() ? 1U : (maxClusterInstances->instanceRange.y + 63U) / 64U, 0U, 1U};
                lists.bladeCullDispatch.createDeviceLocal(vulkanDevice.physical(), device, &bladeDispatch, sizeof(bladeDispatch), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                const std::array<GrassBladeDispatchData, 3> emptyDispatches{};
                lists.dispatchIndirect.createDeviceLocal(vulkanDevice.physical(), device, emptyDispatches.data(), sizeof(emptyDispatches), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                for (Buffer* buffer : {&lists.mainVisibleInstances, &lists.shadowVisibleInstances, &lists.velocityVisibleInstances}) buffer->createDeviceLocal(vulkanDevice.physical(), device, emptyVisibleGrass.data(), sizeof(std::uint32_t) * grassCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                std::array<std::uint32_t, 3> zeroStreamCounts{};
                lists.classifyCounts.createDeviceLocal(vulkanDevice.physical(), device, zeroStreamCounts.data(), sizeof(zeroStreamCounts), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                std::vector<std::uint32_t> zeroBins(clusterCapacity, 0U);
                for (uint32_t stream = 0; stream < 3; ++stream) {
                    for (Buffer* buffer : {&lists.binCounts[stream], &lists.binOffsets[stream], &lists.binCursors[stream]}) buffer->createDeviceLocal(vulkanDevice.physical(), device, zeroBins.data(), sizeof(std::uint32_t) * clusterCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                    lists.drawInstances[stream].createDeviceLocal(vulkanDevice.physical(), device, emptyVisibleGrass.data(), sizeof(std::uint32_t) * grassCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                }
                grassClassifyUniformBuffers[frame].createHostVisible(vulkanDevice.physical(), device, sizeof(GrassClassifyUniformData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, vulkanDevice.allocator());
                grassPackedCullUniformBuffers[frame].createHostVisible(vulkanDevice.physical(), device, sizeof(GrassPackedCullUniformData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, vulkanDevice.allocator());
                for (Buffer& buffer : grassPackedStreamUniformBuffers[frame]) buffer.createHostVisible(vulkanDevice.physical(), device, sizeof(GrassPackedStreamUniformData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, vulkanDevice.allocator());
                for (Buffer* buffer : {&lists.mainIndirect, &lists.shadowIndirect, &lists.velocityIndirect})
                    buffer->createDeviceLocal(vulkanDevice.physical(), device, emptyGrassList.data(), sizeof(VkDrawIndexedIndirectCommand) * emptyGrassList.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                for (Buffer* buffer : {&lists.mainDrawCount, &lists.shadowDrawCount, &lists.velocityDrawCount})
                    buffer->createDeviceLocal(vulkanDevice.physical(), device, &zero, sizeof(zero), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                auto& sceneLists = sceneGrassRenderLists[frame];
                sceneLists.visibleInstances.createDeviceLocal(vulkanDevice.physical(), device, emptyVisibleGrass.data(), sizeof(std::uint32_t) * grassCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                sceneLists.visibleCount.createDeviceLocal(vulkanDevice.physical(), device, zeroDispatchCounts.data(), sizeof(zeroDispatchCounts), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                sceneLists.visibleClusters.createDeviceLocal(vulkanDevice.physical(), device, emptyVisibleClusters.data(), sizeof(std::uint32_t) * clusterCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                sceneLists.visibleClusterCount.createDeviceLocal(vulkanDevice.physical(), device, &zero, sizeof(zero), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                sceneLists.bladeCullDispatch.createDeviceLocal(vulkanDevice.physical(), device, &bladeDispatch, sizeof(bladeDispatch), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                const std::array<GrassBladeDispatchData, 3> emptySceneDispatches{};
                sceneLists.dispatchIndirect.createDeviceLocal(vulkanDevice.physical(), device, emptySceneDispatches.data(), sizeof(emptySceneDispatches), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                sceneLists.mainVisibleInstances.createDeviceLocal(vulkanDevice.physical(), device, emptyVisibleGrass.data(), sizeof(std::uint32_t) * grassCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                sceneLists.classifyCounts.createDeviceLocal(vulkanDevice.physical(), device, zeroStreamCounts.data(), sizeof(zeroStreamCounts), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                for (Buffer* buffer : {&sceneLists.binCounts, &sceneLists.binOffsets, &sceneLists.binCursors}) buffer->createDeviceLocal(vulkanDevice.physical(), device, zeroBins.data(), sizeof(std::uint32_t) * clusterCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                sceneLists.drawInstances.createDeviceLocal(vulkanDevice.physical(), device, emptyVisibleGrass.data(), sizeof(std::uint32_t) * grassCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                sceneGrassClassifyUniformBuffers[frame].createHostVisible(vulkanDevice.physical(), device, sizeof(GrassClassifyUniformData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, vulkanDevice.allocator());
                sceneGrassPackedCullUniformBuffers[frame].createHostVisible(vulkanDevice.physical(), device, sizeof(GrassPackedCullUniformData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, vulkanDevice.allocator());
                for (Buffer& buffer : sceneGrassPackedStreamUniformBuffers[frame]) buffer.createHostVisible(vulkanDevice.physical(), device, sizeof(GrassPackedStreamUniformData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, vulkanDevice.allocator());
                sceneLists.mainIndirect.createDeviceLocal(vulkanDevice.physical(), device, emptyGrassList.data(), sizeof(VkDrawIndexedIndirectCommand) * emptyGrassList.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                sceneLists.mainDrawCount.createDeviceLocal(vulkanDevice.physical(), device, &zero, sizeof(zero), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                foliageIndirectBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device, emptyCommands.data(),
                    sizeof(VkDrawIndexedIndirectCommand) * genericCapacity,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                sceneIndirectBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device, emptyCommands.data(),
                    sizeof(VkDrawIndexedIndirectCommand) * genericCapacity,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                sceneFoliageIndirectBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device, emptyCommands.data(),
                    sizeof(VkDrawIndexedIndirectCommand) * genericCapacity,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                std::vector<VkDrawIndexedIndirectCommand> emptyShadowCommands(
                    genericCapacity * ShadowMap::MaxPageUpdatesPerFrame);
                std::vector<std::uint32_t> emptyShadowCandidates(
                    genericCapacity * ShadowMap::ClipLevelCount);
                shadowCandidateBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device,
                    emptyShadowCandidates.data(), sizeof(std::uint32_t) * emptyShadowCandidates.size(),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
                std::array<std::uint32_t, ShadowMap::ClipLevelCount> zeroCandidateCounts{};
                shadowCandidateCountBuffers[frame].createDeviceLocal(vulkanDevice.physical(), device,
                    zeroCandidateCounts.data(), sizeof(zeroCandidateCounts),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
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
            // Per frame: 4 legacy sets, game cluster/blade/classify sets,
            // 14 per-frame shared sets (including four dispatch builders),
            // plus 24 stream-builder sets.
            // Keep this in lockstep with allocateGrassSets below.
            constexpr uint32_t grassSetCount = MAX_FRAMES_IN_FLIGHT * 38;
            const uint32_t imageDescriptors = hiZBuffer.mipCount() + cullingSetCount;
            const VkDescriptorPoolSize poolSizes[] = {
                {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, imageDescriptors},
                {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, hiZBuffer.mipCount()},
                {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cullingSetCount * 5 + instanceCullSetCount * 3 + MAX_FRAMES_IN_FLIGHT * 128},
                {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, cullingSetCount + MAX_FRAMES_IN_FLIGHT * 24},
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
            allocateGrassSets(grassDispatchBuildDescriptorSetLayout, grassVisibleDispatchBuildSets);
            allocateGrassSets(grassDispatchBuildDescriptorSetLayout, grassStreamDispatchBuildSets);
            allocateGrassSets(grassDispatchBuildDescriptorSetLayout, sceneGrassVisibleDispatchBuildSets);
            allocateGrassSets(grassDispatchBuildDescriptorSetLayout, sceneGrassStreamDispatchBuildSets);
            allocateGrassSets(grassPrefixDescriptorSetLayout, grassPrefixSets);
            allocateGrassSets(grassScatterDescriptorSetLayout, grassScatterSets);
            allocateGrassSets(grassFinalizeDescriptorSetLayout, grassFinalizeSets);
            allocateGrassSets(grassPackedCullDescriptorSetLayout, grassPackedCullSets);
            allocateGrassSets(grassBladeCullDescriptorSetLayout, grassBladeCullSets);
            allocateGrassSets(grassClassifyDescriptorSetLayout, grassClassifySets);
            allocateGrassSets(grassPackedCullDescriptorSetLayout, sceneGrassPackedCullSets);
            allocateGrassSets(grassBladeCullDescriptorSetLayout, sceneGrassBladeCullSets);
            allocateGrassSets(grassClassifyDescriptorSetLayout, sceneGrassClassifySets);
            for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
                for (uint32_t stream = 0; stream < 3; ++stream) {
                    const auto allocateOne = [&](VkDescriptorSetLayout layout, VkDescriptorSet& set) {
                        allocateInfo.descriptorSetCount = 1; allocateInfo.pSetLayouts = &layout;
                        if (vkAllocateDescriptorSets(device, &allocateInfo, &set) != VK_SUCCESS) throw std::runtime_error("Could not allocate packed grass descriptor set");
                    };
                    allocateOne(grassPackedBinDescriptorSetLayout, grassPackedBinSets[frame][stream]);
                    allocateOne(grassPrefixDescriptorSetLayout, grassPackedPrefixSets[frame][stream]);
                    allocateOne(grassPackedScatterDescriptorSetLayout, grassPackedScatterSets[frame][stream]);
                    allocateOne(grassPackedFinalizeDescriptorSetLayout, grassPackedFinalizeSets[frame][stream]);
                    allocateOne(grassPackedBinDescriptorSetLayout, sceneGrassPackedBinSets[frame][stream]);
                    allocateOne(grassPrefixDescriptorSetLayout, sceneGrassPackedPrefixSets[frame][stream]);
                    allocateOne(grassPackedScatterDescriptorSetLayout, sceneGrassPackedScatterSets[frame][stream]);
                    allocateOne(grassPackedFinalizeDescriptorSetLayout, sceneGrassPackedFinalizeSets[frame][stream]);
                }
            }
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
                    infos.reserve(storageBuffers.size() + (uniformBuffer != VK_NULL_HANDLE ? 1 : 0));
                    for (const VkBuffer buffer : storageBuffers) infos.push_back({buffer, 0, VK_WHOLE_SIZE});
                    if (uniformBuffer != VK_NULL_HANDLE) infos.push_back({uniformBuffer, 0, VK_WHOLE_SIZE});
                    std::vector<VkWriteDescriptorSet> writes(infos.size());
                    for (std::uint32_t binding = 0; binding < writes.size(); ++binding) {
                        writes[binding] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                            .dstSet = set, .dstBinding = binding, .descriptorCount = 1,
                            .descriptorType = uniformBuffer != VK_NULL_HANDLE && binding + 1 == writes.size()
                                ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            .pBufferInfo = &infos[binding]};
                    }
                    vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()),
                                           writes.data(), 0, nullptr);
                };
                auto& packedLists = grassRenderLists[frame];
                updateGrassSet(grassVisibleDispatchBuildSets[frame], {packedLists.visibleCount.handle(), packedLists.dispatchIndirect.handle()}, VK_NULL_HANDLE);
                updateGrassSet(grassStreamDispatchBuildSets[frame], {packedLists.classifyCounts.handle(), packedLists.dispatchIndirect.handle()}, VK_NULL_HANDLE);
                updateGrassSet(grassPackedCullSets[frame], {grassClusterBuffers[frame].handle(), packedLists.visibleClusters.handle(), packedLists.visibleClusterCount.handle(), packedLists.bladeCullDispatch.handle()}, grassPackedCullUniformBuffers[frame].handle());
                updateGrassSet(grassBladeCullSets[frame], {generatedGrassInstanceBuffers[frame].handle(), grassClusterBuffers[frame].handle(), packedLists.visibleClusters.handle(), packedLists.visibleClusterCount.handle(), packedLists.visibleInstances.handle(), packedLists.visibleCount.handle()}, grassPackedCullUniformBuffers[frame].handle());
                updateGrassSet(grassClassifySets[frame], {generatedGrassInstanceBuffers[frame].handle(), grassClusterBuffers[frame].handle(), packedLists.visibleInstances.handle(), packedLists.mainVisibleInstances.handle(), packedLists.shadowVisibleInstances.handle(), packedLists.velocityVisibleInstances.handle(), packedLists.classifyCounts.handle(), packedLists.visibleCount.handle()}, grassClassifyUniformBuffers[frame].handle());
                const std::array<VkBuffer, 3> classified{packedLists.mainVisibleInstances.handle(), packedLists.shadowVisibleInstances.handle(), packedLists.velocityVisibleInstances.handle()};
                const std::array<VkBuffer, 3> indirect{packedLists.mainIndirect.handle(), packedLists.shadowIndirect.handle(), packedLists.velocityIndirect.handle()};
                const std::array<VkBuffer, 3> drawCounts{packedLists.mainDrawCount.handle(), packedLists.shadowDrawCount.handle(), packedLists.velocityDrawCount.handle()};
                for (uint32_t stream = 0; stream < 3; ++stream) {
                    const VkBuffer uniform = grassPackedStreamUniformBuffers[frame][stream].handle();
                    updateGrassSet(grassPackedBinSets[frame][stream], {generatedGrassInstanceBuffers[frame].handle(), classified[stream], packedLists.classifyCounts.handle(), packedLists.binCounts[stream].handle()}, uniform);
                    updateGrassSet(grassPackedPrefixSets[frame][stream], {packedLists.binCounts[stream].handle(), packedLists.binOffsets[stream].handle()}, uniform);
                    updateGrassSet(grassPackedScatterSets[frame][stream], {generatedGrassInstanceBuffers[frame].handle(), classified[stream], packedLists.classifyCounts.handle(), packedLists.binOffsets[stream].handle(), packedLists.binCursors[stream].handle(), packedLists.drawInstances[stream].handle()}, uniform);
                    updateGrassSet(grassPackedFinalizeSets[frame][stream], {grassClusterBuffers[frame].handle(), packedLists.binCounts[stream].handle(), packedLists.binOffsets[stream].handle(), indirect[stream], drawCounts[stream]}, uniform);
                }
                auto& scenePackedLists = sceneGrassRenderLists[frame];
                updateGrassSet(sceneGrassVisibleDispatchBuildSets[frame], {scenePackedLists.visibleCount.handle(), scenePackedLists.dispatchIndirect.handle()}, VK_NULL_HANDLE);
                updateGrassSet(sceneGrassStreamDispatchBuildSets[frame], {scenePackedLists.classifyCounts.handle(), scenePackedLists.dispatchIndirect.handle()}, VK_NULL_HANDLE);
                updateGrassSet(sceneGrassPackedCullSets[frame], {grassClusterBuffers[frame].handle(), scenePackedLists.visibleClusters.handle(), scenePackedLists.visibleClusterCount.handle(), scenePackedLists.bladeCullDispatch.handle()}, sceneGrassPackedCullUniformBuffers[frame].handle());
                updateGrassSet(sceneGrassBladeCullSets[frame], {generatedGrassInstanceBuffers[frame].handle(), grassClusterBuffers[frame].handle(), scenePackedLists.visibleClusters.handle(), scenePackedLists.visibleClusterCount.handle(), scenePackedLists.visibleInstances.handle(), scenePackedLists.visibleCount.handle()}, sceneGrassPackedCullUniformBuffers[frame].handle());
                const VkBuffer sceneUniform = sceneGrassPackedStreamUniformBuffers[frame][0].handle();
                updateGrassSet(sceneGrassClassifySets[frame], {generatedGrassInstanceBuffers[frame].handle(), grassClusterBuffers[frame].handle(), scenePackedLists.visibleInstances.handle(), scenePackedLists.mainVisibleInstances.handle(), scenePackedLists.mainVisibleInstances.handle(), scenePackedLists.mainVisibleInstances.handle(), scenePackedLists.classifyCounts.handle(), scenePackedLists.visibleCount.handle()}, sceneGrassClassifyUniformBuffers[frame].handle());
                updateGrassSet(sceneGrassPackedBinSets[frame][0], {generatedGrassInstanceBuffers[frame].handle(), scenePackedLists.mainVisibleInstances.handle(), scenePackedLists.classifyCounts.handle(), scenePackedLists.binCounts.handle()}, sceneUniform);
                updateGrassSet(sceneGrassPackedPrefixSets[frame][0], {scenePackedLists.binCounts.handle(), scenePackedLists.binOffsets.handle()}, sceneUniform);
                updateGrassSet(sceneGrassPackedScatterSets[frame][0], {generatedGrassInstanceBuffers[frame].handle(), scenePackedLists.mainVisibleInstances.handle(), scenePackedLists.classifyCounts.handle(), scenePackedLists.binOffsets.handle(), scenePackedLists.binCursors.handle(), scenePackedLists.drawInstances.handle()}, sceneUniform);
                updateGrassSet(sceneGrassPackedFinalizeSets[frame][0], {grassClusterBuffers[frame].handle(), scenePackedLists.binCounts.handle(), scenePackedLists.binOffsets.handle(), scenePackedLists.mainIndirect.handle(), scenePackedLists.mainDrawCount.handle()}, sceneUniform);
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
                    const Buffer& candidateBuffer;
                    const Buffer& candidateCountBuffer;
                };
                const auto updateCullingSet = [&](const CullingSetUpdate& update) {
                    const VkDescriptorBufferInfo objectInfo{
                        cullingObjectBuffers[frame].handle(), 0, VK_WHOLE_SIZE};
                    const VkDescriptorBufferInfo indirectInfo{update.indirectBuffer.handle(), 0, VK_WHOLE_SIZE};
                    const VkDescriptorBufferInfo countInfo{update.countBuffer.handle(), 0, VK_WHOLE_SIZE};
                    const VkDescriptorBufferInfo uniformInfo{update.uniformBuffer.handle(), 0, sizeof(Culling::CullingUniformData)};
                    const VkDescriptorBufferInfo candidateInfo{update.candidateBuffer.handle(), 0, VK_WHOLE_SIZE};
                    const VkDescriptorBufferInfo candidateCountInfo{update.candidateCountBuffer.handle(), 0, VK_WHOLE_SIZE};
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
                    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[5].pBufferInfo = &candidateInfo;
                    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[6].pBufferInfo = &candidateCountInfo;
                    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
                };
                VkDescriptorSet cameraCullSet = cullSets[frame];
                VkDescriptorSet foliageCullSet = cullSets[MAX_FRAMES_IN_FLIGHT + frame];
                VkDescriptorSet sceneCullSet = cullSets[MAX_FRAMES_IN_FLIGHT * 2 + frame];
                VkDescriptorSet sceneFoliageCullSet = cullSets[MAX_FRAMES_IN_FLIGHT * 3 + frame];
                VkDescriptorSet shadowCullSet = cullSets[MAX_FRAMES_IN_FLIGHT * 4 + frame];
                const auto& candidates = shadowCandidateBuffers[frame];
                const auto& candidateCounts = shadowCandidateCountBuffers[frame];
                updateCullingSet({cameraCullSet, indirectBuffers[frame], drawCountBuffers[frame],
                                  cullingUniformBuffers[frame], candidates, candidateCounts});
                updateCullingSet({foliageCullSet, foliageIndirectBuffers[frame], foliageDrawCountBuffers[frame],
                                  foliageCullingUniformBuffers[frame], candidates, candidateCounts});
                updateCullingSet({shadowCullSet, shadowIndirectBuffers[frame], shadowDrawCountBuffers[frame],
                                  shadowCullingUniformBuffers[frame], candidates, candidateCounts});
                updateCullingSet({sceneCullSet, sceneIndirectBuffers[frame], sceneDrawCountBuffers[frame],
                                  sceneCullingUniformBuffers[frame], candidates, candidateCounts});
                updateCullingSet({sceneFoliageCullSet, sceneFoliageIndirectBuffers[frame], sceneFoliageDrawCountBuffers[frame],
                                  sceneFoliageCullingUniformBuffers[frame], candidates, candidateCounts});
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
                    shadowIndirectBuffers[frame].handle(), shadowDrawCountBuffers[frame].handle(), objectCount,
                    shadowCandidateCountBuffers[frame].handle());
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
            for (Buffer& buffer : shadowCandidateBuffers) buffer.destroy();
            for (Buffer& buffer : shadowCandidateCountBuffers) buffer.destroy();
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
            for (Buffer& buffer : grassClassifyUniformBuffers) buffer.destroy();
            for (Buffer& buffer : grassPackedCullUniformBuffers) buffer.destroy();
            for (auto& streamUniforms : grassPackedStreamUniformBuffers) {
                for (Buffer& buffer : streamUniforms) buffer.destroy();
            }
            for (Buffer& buffer : sceneGrassClassifyUniformBuffers) buffer.destroy();
            for (Buffer& buffer : sceneGrassPackedCullUniformBuffers) buffer.destroy();
            for (auto& streamUniforms : sceneGrassPackedStreamUniformBuffers) {
                for (Buffer& buffer : streamUniforms) buffer.destroy();
            }
            for (GrassRenderLists& lists : grassRenderLists) {
                lists.visibleInstances.destroy();
                lists.visibleCount.destroy();
                lists.visibleClusters.destroy(); lists.visibleClusterCount.destroy(); lists.bladeCullDispatch.destroy();
                lists.dispatchIndirect.destroy();
                lists.mainVisibleInstances.destroy();
                lists.shadowVisibleInstances.destroy();
                lists.velocityVisibleInstances.destroy();
                lists.classifyCounts.destroy();
                for (Buffer& buffer : lists.binCounts) buffer.destroy();
                for (Buffer& buffer : lists.binOffsets) buffer.destroy();
                for (Buffer& buffer : lists.binCursors) buffer.destroy();
                for (Buffer& buffer : lists.drawInstances) buffer.destroy();
                lists.mainIndirect.destroy();
                lists.mainDrawCount.destroy();
                lists.shadowIndirect.destroy();
                lists.shadowDrawCount.destroy();
                lists.velocityIndirect.destroy();
                lists.velocityDrawCount.destroy();
            }
            for (SceneGrassRenderLists& lists : sceneGrassRenderLists) {
                lists.visibleInstances.destroy();
                lists.visibleCount.destroy();
                lists.visibleClusters.destroy(); lists.visibleClusterCount.destroy(); lists.bladeCullDispatch.destroy();
                lists.dispatchIndirect.destroy();
                lists.mainVisibleInstances.destroy();
                lists.classifyCounts.destroy();
                lists.binCounts.destroy(); lists.binOffsets.destroy(); lists.binCursors.destroy(); lists.drawInstances.destroy();
                lists.mainIndirect.destroy();
                lists.mainDrawCount.destroy();
            }
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
            if (grassDispatchBuildPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, grassDispatchBuildPipeline, nullptr);
            if (grassPrefixPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, grassPrefixPipeline, nullptr);
            if (grassScatterPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, grassScatterPipeline, nullptr);
            if (grassFinalizePipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, grassFinalizePipeline, nullptr);
            if (grassPackedCullPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, grassPackedCullPipeline, nullptr);
            if (grassBladeCullPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, grassBladeCullPipeline, nullptr);
            if (grassClassifyPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, grassClassifyPipeline, nullptr);
            if (grassPackedBinPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, grassPackedBinPipeline, nullptr);
            if (grassPackedPrefixPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, grassPackedPrefixPipeline, nullptr);
            if (grassPackedScatterPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, grassPackedScatterPipeline, nullptr);
            if (grassPackedFinalizePipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, grassPackedFinalizePipeline, nullptr);
            if (hiZCopyPipelineLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, hiZCopyPipelineLayout, nullptr);
}
            if (hiZReducePipelineLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, hiZReducePipelineLayout, nullptr);
}
            if (cullingPipelineLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, cullingPipelineLayout, nullptr);
}
            if (instanceCullingPipelineLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, instanceCullingPipelineLayout, nullptr);
}
            if (grassBuildPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, grassBuildPipelineLayout, nullptr);
            if (grassDispatchBuildPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, grassDispatchBuildPipelineLayout, nullptr);
            if (grassPrefixPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, grassPrefixPipelineLayout, nullptr);
            if (grassScatterPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, grassScatterPipelineLayout, nullptr);
            if (grassFinalizePipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, grassFinalizePipelineLayout, nullptr);
            if (grassPackedCullPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, grassPackedCullPipelineLayout, nullptr);
            if (grassBladeCullPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, grassBladeCullPipelineLayout, nullptr);
            if (grassClassifyPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, grassClassifyPipelineLayout, nullptr);
            if (grassPackedBinPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, grassPackedBinPipelineLayout, nullptr);
            if (grassPackedScatterPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, grassPackedScatterPipelineLayout, nullptr);
            if (grassPackedFinalizePipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, grassPackedFinalizePipelineLayout, nullptr);
            if (hiZCopyDescriptorSetLayout != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device, hiZCopyDescriptorSetLayout, nullptr);
}
            if (hiZReduceDescriptorSetLayout != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device, hiZReduceDescriptorSetLayout, nullptr);
}
            if (cullingDescriptorSetLayout != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device, cullingDescriptorSetLayout, nullptr);
}
            if (instanceCullingDescriptorSetLayout != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device, instanceCullingDescriptorSetLayout, nullptr);
}
            if (grassBuildDescriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, grassBuildDescriptorSetLayout, nullptr);
            if (grassDispatchBuildDescriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, grassDispatchBuildDescriptorSetLayout, nullptr);
            if (grassPrefixDescriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, grassPrefixDescriptorSetLayout, nullptr);
            if (grassScatterDescriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, grassScatterDescriptorSetLayout, nullptr);
            if (grassFinalizeDescriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, grassFinalizeDescriptorSetLayout, nullptr);
            if (grassPackedCullDescriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, grassPackedCullDescriptorSetLayout, nullptr);
            if (grassBladeCullDescriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, grassBladeCullDescriptorSetLayout, nullptr);
            if (grassClassifyDescriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, grassClassifyDescriptorSetLayout, nullptr);
            if (grassPackedBinDescriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, grassPackedBinDescriptorSetLayout, nullptr);
            if (grassPackedScatterDescriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, grassPackedScatterDescriptorSetLayout, nullptr);
            if (grassPackedFinalizeDescriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, grassPackedFinalizeDescriptorSetLayout, nullptr);
            cullingDescriptorPool = VK_NULL_HANDLE; hiZCopyPipeline = hiZReducePipeline = cullingPipeline = VK_NULL_HANDLE;
            instanceCullingPipeline = VK_NULL_HANDLE;
            grassBuildPipeline = grassPrefixPipeline = grassScatterPipeline = grassFinalizePipeline = VK_NULL_HANDLE;
            grassPackedCullPipeline = grassBladeCullPipeline = grassClassifyPipeline = VK_NULL_HANDLE;
            grassPackedBinPipeline = grassPackedPrefixPipeline = grassPackedScatterPipeline = grassPackedFinalizePipeline = VK_NULL_HANDLE;
            hiZCopyPipelineLayout = hiZReducePipelineLayout = cullingPipelineLayout = VK_NULL_HANDLE;
            instanceCullingPipelineLayout = VK_NULL_HANDLE;
            grassBuildPipelineLayout = grassPrefixPipelineLayout = grassScatterPipelineLayout = grassFinalizePipelineLayout = VK_NULL_HANDLE;
            grassPackedCullPipelineLayout = grassBladeCullPipelineLayout = grassClassifyPipelineLayout = VK_NULL_HANDLE;
            grassPackedBinPipelineLayout = grassPackedScatterPipelineLayout = grassPackedFinalizePipelineLayout = VK_NULL_HANDLE;
            hiZCopyDescriptorSetLayout = hiZReduceDescriptorSetLayout = cullingDescriptorSetLayout = VK_NULL_HANDLE;
            instanceCullingDescriptorSetLayout = VK_NULL_HANDLE;
            grassBuildDescriptorSetLayout = grassPrefixDescriptorSetLayout = grassScatterDescriptorSetLayout = grassFinalizeDescriptorSetLayout = VK_NULL_HANDLE;
            grassPackedCullDescriptorSetLayout = grassBladeCullDescriptorSetLayout = grassClassifyDescriptorSetLayout = VK_NULL_HANDLE;
            grassPackedBinDescriptorSetLayout = grassPackedScatterDescriptorSetLayout = grassPackedFinalizeDescriptorSetLayout = VK_NULL_HANDLE;
            hiZBuffer.destroy(); gpuObjects.clear(); hiZValid = false;
        }

        void createFramebuffers() {
            const VkExtent2D extent = swapchain.extent();
            VkImageView msaaAttachments[] = {
                msaa.colorImageView(), depthBuffer.imageView(), hdrBuffer.imageView(), hiZDepthBuffer.imageView()
            };
            VkImageView directAttachments[] = {
                hdrBuffer.imageView(), depthBuffer.imageView()
            };

            VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            framebufferInfo.renderPass = forwardPass.renderPass();
            framebufferInfo.attachmentCount = msaa.enabled() ? 4u : 2u;
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
                GraphicsPipelineOptions grassVelocityOptions = foliageVelocityOptions;
                grassVelocityOptions.shader = "shaders/grass_velocity.spv";
                grassVelocityPipeline.create(device, grassVelocityOptions);

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
            grassVelocityPipeline.destroy();
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
            // then single-sample color and depth resolve targets.
            VkImageView msaaAttachments[] = {
                sceneViewportTarget.msaaColorImageView(), sceneViewportTarget.depth().imageView(),
                sceneViewportTarget.color().imageView(), sceneViewportTarget.resolvedDepth().imageView()};
            VkImageView directAttachments[] = {
                sceneViewportTarget.color().imageView(), sceneViewportTarget.depth().imageView()};
            VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            info.renderPass = forwardPass.renderPass();
            info.attachmentCount = msaa.enabled() ? 4u : 2u;
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
