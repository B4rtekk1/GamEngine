void refreshCullingBatchObjects() {
    constexpr std::size_t modelDiagonalStride = 5;
    const std::uint32_t objectCount = static_cast<std::uint32_t>(instanceBatches.size());
    gpuObjects.resize(objectCount);
    for (std::uint32_t i = 0; i < objectCount; ++i) {
        InstanceBatch& batch = instanceBatches[i];
        auto& object = gpuObjects[i];
        object.model = {};
        object.model.data[0] = 1.0F;
        object.model.data[modelDiagonalStride] = 1.0F;
        object.model.data[modelDiagonalStride * 2] = 1.0F;
        object.model.data[modelDiagonalStride * 3] = 1.0F;
        const AABB& bounds = batch.worldBounds;
        object.localAabbMin = {bounds.min.x(), bounds.min.y(), bounds.min.z(), 0.0F};
        object.localAabbMax = {bounds.max.x(), bounds.max.y(), bounds.max.z(), 0.0F};
        object.indexCount = batch.indexCount;
        object.instanceCount = batch.instanceCount;
        object.firstIndex = batch.firstIndex;
        object.vertexOffset = 0;
        object.firstInstance = batch.firstInstance;
        object.castShadow = batch.castShadow ? 1U : 0U;
        object.shadowCacheMode = static_cast<std::uint32_t>(batch.shadowCacheMode);
        object.twoSided = batch.foliagePipeline ? 1U : 0U;
        object.shader = batch.shaderSlot;
        object.lod1IndexCount = batch.lod1IndexCount;
        object.lod2IndexCount = batch.lod2IndexCount;
        object.lod1DistanceSq = batch.lod1Distance * batch.lod1Distance;
        object.lod2DistanceSq = batch.lod2Distance * batch.lod2Distance;
        // Per-meshlet VSM rasterization: undisplaced sections, opaque
        // one-sided or foliage, whose meshlet triangles are exactly their
        // LOD0 indices in the shared index heap.
        // Foliage batches form the two-sided stream, whose casters keep the
        // alpha test (shadow_meshlet_mesh.slang, SHADOW_MESH_ALPHA_TEST).
        const bool meshletShadows = batch.meshletCount != 0 && meshletIndexedDrawsSupported() &&
                                    !batch.displacedGeometry &&
                                    (batch.foliagePipeline || batch.alphaMode == AlphaMode::Opaque);
        object.firstMeshlet = meshletShadows ? batch.firstMeshlet : 0U;
        object.meshletCount = meshletShadows ? batch.meshletCount : 0U;
    }
    // Instances may have changed slots on a reused allocation.
    if (meshletBinBudget != 0) {
        computeMeshletBinOffsets();
        uploadMeshletBinOffsets();
    }
    // Pack the binned streams: each slot holds exactly its objects.
    std::array<std::array<std::uint32_t, Culling::DrawBinCount>, 2> binCounts{};
    for (const auto& object : gpuObjects)
        if (object.shader < Culling::DrawBinCount)
            ++binCounts[object.twoSided != 0U ? 1 : 0][object.shader];
    for (std::size_t stream = 0; stream < drawBinOffsets.size(); ++stream) {
        std::uint32_t offset = 0;
        for (std::uint32_t bin = 0; bin < Culling::DrawBinOffsetCount; ++bin) {
            drawBinOffsets[stream][bin] = offset;
            if (bin < Culling::DrawBinCount)
                offset += binCounts[stream][bin];
        }
    }
    const Culling::GPUObjectData emptyObject{};
    for (Buffer& buffer : cullingObjectBuffers) {
        if (buffer.handle() == VK_NULL_HANDLE)
            continue;
        buffer.update(objectCount == 0 ? &emptyObject : gpuObjects.data(),
                      sizeof(Culling::GPUObjectData) * std::max(1U, objectCount));
    }
    for (RenderableRecord& record : renderables)
        record.cullingDirtyFrames = 0;
}

struct ShadowPageRequirements {
    std::array<std::uint32_t, 2> casters{};
    std::array<std::uint32_t, 2> instances{};
};

// What one VSM page can receive per category: culling keeps only shadow
// casters of the pass's category (gpu_culling.slang), and the compact
// transforms fall back to the scene streams beyond MaxCasterInstancesPerPage.
[[nodiscard]] ShadowPageRequirements shadowPageRequirements() const {
    std::array<std::uint64_t, 2> casters{};
    std::array<std::uint64_t, 2> instances{};
    for (const InstanceBatch& batch : instanceBatches) {
        if (!batch.castShadow)
            continue;
        const std::size_t category = batch.foliagePipeline ? 1 : 0;
        ++casters[category];
        instances[category] += batch.instanceCount;
    }
    ShadowPageRequirements result;
    for (std::size_t category = 0; category < 2; ++category) {
        result.casters[category] = static_cast<std::uint32_t>(std::max<std::uint64_t>(1, casters[category]));
        result.instances[category] = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(
            instances[category], 1, ShadowMap::MaxCasterInstancesPerPage));
    }
    return result;
}

// Binned indirect buffers hold twice the object count (see createCullingResources).
static constexpr std::size_t BinnedIndirectSlack = 2;

// Packs the meshlet draw stream: each material slot reserves the meshlets of
// the instances drawn with it (plus headroom for merged/added instances),
// instead of every visible meshlet per slot (x32).
void computeMeshletBinOffsets() {
    std::array<std::uint64_t, Culling::DrawBinCount> counts{};
    const auto& meshes = sceneGpu.database.meshes();
    const auto& databaseMaterials = sceneGpu.database.materials();
    for (const auto& instance : sceneGpu.database.instances()) {
        if (!instance.alive || instance.meshId >= meshes.size() || instance.materialId >= databaseMaterials.size())
            continue;
        const std::uint32_t slot = databaseMaterials[instance.materialId].shaderSlot;
        if (slot < Culling::DrawBinCount)
            counts[slot] += meshes[instance.meshId].meshletCount;
    }
    const auto pack = [&](const bool headroom) {
        std::uint64_t offset = 0;
        for (std::uint32_t bin = 0; bin < Culling::DrawBinOffsetCount; ++bin) {
            meshletBinOffsets[bin] = static_cast<std::uint32_t>(std::min<std::uint64_t>(offset, meshletBinBudget));
            if (bin < Culling::DrawBinCount)
                offset += counts[bin] + (headroom && counts[bin] != 0 ? counts[bin] / 8U + 64U : 0U);
        }
        return offset <= meshletBinBudget;
    };
    if (!pack(true))
        static_cast<void>(pack(false));
}

[[nodiscard]] VkDeviceSize meshletCommandOffset(const std::uint32_t slot) const noexcept {
    if (slot >= Culling::DrawBinCount)
        return 0;
    return static_cast<VkDeviceSize>(meshletBinOffsets[slot]) * sizeof(VkDrawIndexedIndirectCommand);
}

void uploadMeshletBinOffsets() {
    for (Buffer& buffer : meshletIndexedDrawCountBuffers) {
        if (buffer.handle() == VK_NULL_HANDLE)
            continue;
        buffer.uploadDeviceLocal(meshletBinOffsets.data(), sizeof(meshletBinOffsets),
                                 sizeof(std::uint32_t) * MaterialProgramSlotCount, commandPool,
                                 vulkanDevice.graphicsQueue());
    }
}

// Byte offset of a material program slot's commands in a binned stream
// (indirectDraws / sceneIndirectDraws: single-sided, foliage lists: two-sided).
[[nodiscard]] VkDeviceSize binnedCommandOffset(const std::uint32_t slot, const bool twoSided) const noexcept {
    if (slot >= Culling::DrawBinCount)
        return 0;
    return static_cast<VkDeviceSize>(drawBinOffsets[twoSided ? 1 : 0][slot]) * sizeof(VkDrawIndexedIndirectCommand);
}

[[nodiscard]] bool canReuseCullingResources() const {
    // Grass has a separate, variable set of compacted scratch lists;
    // keep its migration boundary explicit until it gets its own heap.
    if (!sceneGpu.grassInstances.empty() || !sceneGpu.grassClusters.empty() || cullingDescriptorPool == VK_NULL_HANDLE)
        return false;
    const VkDeviceSize objectBytes = sizeof(Culling::GPUObjectData) * std::max<std::size_t>(1, instanceBatches.size());
    const VkDeviceSize visibleBytes =
        sizeof(std::uint32_t) * std::max<std::size_t>(1, sceneGpu.database.instances().size());
    if (!meshletForwardPathActive || !vulkanDevice.supportsMeshShaders()) {
        if (meshletVisibleCapacity != 1)
            return false;
    } else {
        std::uint64_t requiredVisibleMeshlets = 0;
        for (const auto& instance : sceneGpu.database.instances()) {
            if (!instance.alive || instance.meshId >= sceneGpu.database.meshes().size())
                continue;
            requiredVisibleMeshlets += sceneGpu.database.meshes()[instance.meshId].meshletCount;
        }
        if (requiredVisibleMeshlets > meshletVisibleCapacity)
            return false;
    }
    const VkDeviceSize indirectBytes = sizeof(VkDrawIndexedIndirectCommand) *
                                       std::max<std::size_t>(1, instanceBatches.size()) * BinnedIndirectSlack;
    // Page strides are fixed when the culling passes are created.
    const ShadowPageRequirements shadowRequired = shadowPageRequirements();
    for (std::size_t category = 0; category < 2; ++category)
        if (shadowRequired.casters[category] > shadowPageCasterCapacity[category] ||
            shadowRequired.instances[category] > shadowPageInstanceCapacity[category])
            return false;
    for (std::uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        if (cullingObjectBuffers[frame].size() < objectBytes || visibleInstanceBuffers[frame].size() < visibleBytes ||
            indirectBuffers[frame].size() < indirectBytes || foliageIndirectBuffers[frame].size() < indirectBytes)
            return false;
    }
    return true;
}

// Hi-Z images stay SHADER_READ_ONLY_OPTIMAL for their whole life. Descriptors
// reference both slots from the first frame, and an invalid pyramid is never
// sampled (hiZValid gates every reader), so its contents need no discard.
// Fixing the layout here keeps per-frame code free of UNDEFINED transitions
// on images that async compute rewrites.
void initializeHiZLayouts() {
    std::array<VkImageMemoryBarrier2, MAX_FRAMES_IN_FLIGHT> barriers{};
    std::uint32_t barrierCount = 0;
    for (const auto& hiZBuffer : hiZBuffers) {
        if (hiZBuffer.image() == VK_NULL_HANDLE)
            continue;
        auto& barrier = barriers[barrierCount++];
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = hiZBuffer.image();
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, hiZBuffer.mipCount(), 0, 1};
    }
    if (barrierCount == 0)
        return;
    const VkCommandBufferAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                               .commandPool = commandPool,
                                               .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                               .commandBufferCount = 1};
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device, &allocate, &commandBuffer) != VK_SUCCESS)
        throw std::runtime_error("Could not allocate Hi-Z initialization commands");
    const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(commandBuffer, &begin);
    const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                      .imageMemoryBarrierCount = barrierCount,
                                      .pImageMemoryBarriers = barriers.data()};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
    vkEndCommandBuffer(commandBuffer);
    const VkCommandBufferSubmitInfo command{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                            .commandBuffer = commandBuffer};
    const VkSubmitInfo2 submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                               .commandBufferInfoCount = 1,
                               .pCommandBufferInfos = &command};
    const VkResult result = vkQueueSubmit2(vulkanDevice.graphicsQueue(), 1, &submit, VK_NULL_HANDLE);
    if (result == VK_SUCCESS)
        vkQueueWaitIdle(vulkanDevice.graphicsQueue());
    vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
    if (result != VK_SUCCESS)
        throw std::runtime_error("Could not initialize Hi-Z layouts");
}

// Set 1 of the mesh-shader forward pipelines; see MeshletDraw.slang. Outlives
// the per-scene culling resources because the forward passes embed it.
void ensureMeshletDrawLayout() {
    if (meshletDrawDescriptorSetLayout != VK_NULL_HANDLE || !vulkanDevice.supportsMeshShaders())
        return;
    VkDescriptorSetLayoutBinding bindings[6]{};
    for (std::uint32_t binding = 0; binding < std::size(bindings); ++binding)
        // Compute: the visibility-buffer resolve fetches the same meshlets.
        bindings[binding] = {binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                             VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    const VkDescriptorSetLayoutCreateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                               .bindingCount = static_cast<std::uint32_t>(std::size(bindings)),
                                               .pBindings = bindings};
    if (vkCreateDescriptorSetLayout(device, &info, nullptr, &meshletDrawDescriptorSetLayout) != VK_SUCCESS)
        throw std::runtime_error("Could not create mesh-shader meshlet descriptor-set layout");
}

void createCullingResources() {
    ensureMeshletDrawLayout();
    auto uploadBatch = uploadContext.beginBatch();
    constexpr std::size_t cullingDescriptorBindingCount = 17;
    const std::uint32_t grassBinCount =
        std::max(1u, static_cast<std::uint32_t>(sceneGpu.database.meshes().size() * 3u));

    hiZValid.fill(false);
    const auto objectCount = static_cast<uint32_t>(instanceBatches.size());
    std::uint64_t requiredVisibleMeshlets = 0;
    if (meshletForwardPathActive && vulkanDevice.supportsMeshShaders()) {
        for (const auto& instance : sceneGpu.database.instances()) {
            if (!instance.alive || instance.meshId >= sceneGpu.database.meshes().size())
                continue;
            requiredVisibleMeshlets += sceneGpu.database.meshes()[instance.meshId].meshletCount;
        }
        if (requiredVisibleMeshlets > std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("Visible meshlet list exceeds uint32 capacity");
    }
    const std::uint32_t meshletCapacity =
        static_cast<std::uint32_t>(std::max<std::uint64_t>(1, requiredVisibleMeshlets));
    meshletVisibleCapacity = meshletCapacity;
    // Packed slots: the meshlets with 1/8 headroom per slot, then the
    // maxDrawCount (meshletCapacity) tail the indirect-count draw may span.
    meshletBinBudget = meshletCapacity + meshletCapacity / 8U + 64U * Culling::DrawBinCount;
    computeMeshletBinOffsets();
    // An empty ECS scene still renders the sky, editor UI, and fallback
    // camera.  Keep the per-view UBOs and descriptor-backed culling
    // resources alive with inert storage entries; logical draw counts
    // below remain zero when there are no renderables.
    // Generic descriptors remain valid in a grass-only scene, but
    // their backing allocations need one inert element. Their draw
    // counts stay zero because objectCount itself remains zero.
    const auto genericCapacity = std::max(1u, objectCount + objectCount / 2u + 1u);
    // Passes and indirect draws require a non-zero physical capacity.
    // Runtime culling still receives objectCount, which may be zero.
    const auto passCapacity = genericCapacity;
    // VSM page lists: per category, the casters with the same 1.5x headroom
    // the camera lists use, instead of every object for both categories.
    const ShadowPageRequirements shadowRequired = shadowPageRequirements();
    for (std::size_t category = 0; category < 2; ++category) {
        const std::uint32_t casters = shadowRequired.casters[category];
        shadowPageCasterCapacity[category] = casters + casters / 2U + 1U;
        const std::uint32_t instances = shadowRequired.instances[category];
        shadowPageInstanceCapacity[category] =
            std::min<std::uint32_t>(instances + instances / 2U + 1U, ShadowMap::MaxCasterInstancesPerPage);
    }
    const auto shadowPassCapacity = shadowPageCasterCapacity[0];
    const auto shadowTwoSidedPassCapacity = shadowPageCasterCapacity[1];
    const auto clusterCountFor = [](const VkExtent2D extent) {
        const std::uint32_t tilesX = (extent.width + ClusterTileSize - 1U) / ClusterTileSize;
        const std::uint32_t tilesY = (extent.height + ClusterTileSize - 1U) / ClusterTileSize;
        return std::max(1U, tilesX * tilesY * ClusterDepthSlices);
    };
    const std::uint32_t gameClusterCount = clusterCountFor(swapchain.extent());
    const std::uint32_t sceneClusterCount = clusterCountFor(sceneViewportTarget.extent());

    // Keep the complete R32F hierarchy out of memory unless the
    // feature can actually consume it. Frustum/GPU culling remains
    // available without this optional occlusion input.
    // VSM receiver marking consumes the previous depth hierarchy even
    // when object occlusion culling itself is disabled.
    const bool allocateHiZ = canUseHiZOcclusionCulling() || optimizationFeatures.shadows;
    if (allocateHiZ) {
        // The game view builds the pyramid on async compute; graphics reads it.
        const std::array hiZFamilies{vulkanDevice.graphicsQueueFamily(), vulkanDevice.computeQueueFamily()};
        const std::span<const std::uint32_t> sharingFamilies =
            vulkanDevice.hasAsyncComputeQueue() && hiZFamilies[0] != hiZFamilies[1]
                ? std::span<const std::uint32_t>(hiZFamilies)
                : std::span<const std::uint32_t>{};
        for (auto& hiZBuffer : hiZBuffers) {
            hiZBuffer.create(vulkanDevice.physical(), device, swapchain.extent().width, swapchain.extent().height,
                             vulkanDevice.allocator(), sharingFamilies);
        }
        initializeHiZLayouts();
    }

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
    const VkDescriptorSetLayoutBinding vsmPageMarkingBindings[] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    layoutInfo.bindingCount = std::size(vsmPageMarkingBindings);
    layoutInfo.pBindings = vsmPageMarkingBindings;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &vsmPageMarkingDescriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("Could not create VSM page-marking descriptor-set layout");
    }
    const VkDescriptorSetLayoutBinding vsmPageCompactBindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    layoutInfo.bindingCount = std::size(vsmPageCompactBindings);
    layoutInfo.pBindings = vsmPageCompactBindings;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &vsmPageCompactDescriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("Could not create VSM page-compaction descriptor-set layout");
    }
    const VkDescriptorSetLayoutBinding cullBindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {12, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {13, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {14, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {15, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    layoutInfo.bindingCount = std::size(cullBindings);
    layoutInfo.pBindings = cullBindings;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &cullingDescriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("Could not create culling descriptor-set layout");
    }
    // Binding 9: the mesh-task commands of shadow_meshlet_build_mesh_commands.
    std::array<VkDescriptorSetLayoutBinding, 10> refineBindings{};
    for (std::uint32_t binding = 0; binding < refineBindings.size(); ++binding)
        refineBindings[binding] = {binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    layoutInfo.bindingCount = static_cast<std::uint32_t>(refineBindings.size());
    layoutInfo.pBindings = refineBindings.data();
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &shadowMeshletRefineDescriptorSetLayout) !=
        VK_SUCCESS)
        throw std::runtime_error("Could not create VSM meshlet-refine descriptor-set layout");
    const VkDescriptorSetLayoutBinding instanceCullBindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    layoutInfo.bindingCount = std::size(instanceCullBindings);
    layoutInfo.pBindings = instanceCullBindings;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &instanceCullingDescriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("Could not create instance-culling descriptor-set layout");
    }
    const VkDescriptorSetLayoutBinding meshletCullBindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {7, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    layoutInfo.bindingCount = std::size(meshletCullBindings);
    layoutInfo.pBindings = meshletCullBindings;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &meshletCullingDescriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("Could not create meshlet-culling descriptor-set layout");
    }
    const VkDescriptorSetLayoutBinding meshletDispatchBindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    layoutInfo.bindingCount = std::size(meshletDispatchBindings);
    layoutInfo.pBindings = meshletDispatchBindings;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &meshletDispatchDescriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("Could not create meshlet-dispatch descriptor-set layout");
    }
    const VkDescriptorSetLayoutBinding meshletIndirectBindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    layoutInfo.bindingCount = std::size(meshletIndirectBindings);
    layoutInfo.pBindings = meshletIndirectBindings;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &meshletIndirectDescriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("Could not create meshlet-indirect descriptor-set layout");
    }
    const VkDescriptorSetLayoutBinding grassBuildBindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    layoutInfo.bindingCount = std::size(grassBuildBindings);
    layoutInfo.pBindings = grassBuildBindings;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &grassBuildDescriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("Could not create grass-build descriptor-set layout");
    }
    const VkDescriptorSetLayoutBinding grassDispatchBuildBindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    layoutInfo.bindingCount = std::size(grassDispatchBuildBindings);
    layoutInfo.pBindings = grassDispatchBuildBindings;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &grassDispatchBuildDescriptorSetLayout) !=
        VK_SUCCESS) {
        throw std::runtime_error("Could not create grass dispatch-build descriptor-set layout");
    }
    const VkDescriptorSetLayoutBinding grassPrefixBindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    layoutInfo.bindingCount = std::size(grassPrefixBindings);
    layoutInfo.pBindings = grassPrefixBindings;
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
    layoutInfo.bindingCount = std::size(grassScatterBindings);
    layoutInfo.pBindings = grassScatterBindings;
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
    layoutInfo.bindingCount = std::size(grassFinalizeBindings);
    layoutInfo.pBindings = grassFinalizeBindings;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &grassFinalizeDescriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("Could not create grass-finalize descriptor-set layout");
    }
    const VkDescriptorSetLayoutBinding grassPackedCullBindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    layoutInfo.bindingCount = std::size(grassPackedCullBindings);
    layoutInfo.pBindings = grassPackedCullBindings;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &grassPackedCullDescriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("Could not create packed grass-cull descriptor-set layout");
    }
    const VkDescriptorSetLayoutBinding grassShadowPageCullBindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    layoutInfo.bindingCount = std::size(grassShadowPageCullBindings);
    layoutInfo.pBindings = grassShadowPageCullBindings;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &grassShadowPageCullDescriptorSetLayout) !=
        VK_SUCCESS)
        throw std::runtime_error("Could not create grass VSM page-cull descriptor-set layout");
    const VkDescriptorSetLayoutBinding grassBladeCullBindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    layoutInfo.bindingCount = std::size(grassBladeCullBindings);
    layoutInfo.pBindings = grassBladeCullBindings;
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
    layoutInfo.bindingCount = std::size(grassClassifyBindings);
    layoutInfo.pBindings = grassClassifyBindings;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &grassClassifyDescriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("Could not create grass-classify descriptor-set layout");
    }
    const VkDescriptorSetLayoutBinding grassPackedBinBindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    layoutInfo.bindingCount = std::size(grassPackedBinBindings);
    layoutInfo.pBindings = grassPackedBinBindings;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &grassPackedBinDescriptorSetLayout) != VK_SUCCESS)
        throw std::runtime_error("Could not create packed grass-bin layout");
    const VkDescriptorSetLayoutBinding grassPackedScatterBindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    layoutInfo.bindingCount = std::size(grassPackedScatterBindings);
    layoutInfo.pBindings = grassPackedScatterBindings;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &grassPackedScatterDescriptorSetLayout) != VK_SUCCESS)
        throw std::runtime_error("Could not create packed grass-scatter layout");
    const VkDescriptorSetLayoutBinding grassPackedFinalizeBindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    layoutInfo.bindingCount = std::size(grassPackedFinalizeBindings);
    layoutInfo.pBindings = grassPackedFinalizeBindings;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &grassPackedFinalizeDescriptorSetLayout) !=
        VK_SUCCESS)
        throw std::runtime_error("Could not create packed grass-finalize layout");
    const VkDescriptorSetLayoutBinding clusteredLightingBindings[] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    layoutInfo.bindingCount = std::size(clusteredLightingBindings);
    layoutInfo.pBindings = clusteredLightingBindings;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &clusteredLightingDescriptorSetLayout) != VK_SUCCESS)
        throw std::runtime_error("Could not create clustered-light descriptor layout");
    const auto createLayout = [&](VkDescriptorSetLayout setLayout, VkPipelineLayout& pipelineLayout,
                                  const VkPushConstantRange* pushConstantRange = nullptr) {
        VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        info.setLayoutCount = 1;
        info.pSetLayouts = &setLayout;
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
    createLayout(vsmPageMarkingDescriptorSetLayout, vsmPageMarkingPipelineLayout);
    createLayout(vsmPageCompactDescriptorSetLayout, vsmPageCompactPipelineLayout);
    const VkPushConstantRange refinePushConstants{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(std::uint32_t) * 4};
    createLayout(shadowMeshletRefineDescriptorSetLayout, shadowMeshletRefinePipelineLayout, &refinePushConstants);
    // mat4 plus page-culling control fields. Keep this synchronized
    // with CullingPushConstants in GPUCullingPass.cpp/.slang.
    const VkPushConstantRange cullingPushConstants{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Mat4) + 28};
    createLayout(cullingDescriptorSetLayout, cullingPipelineLayout, &cullingPushConstants);
    createLayout(instanceCullingDescriptorSetLayout, instanceCullingPipelineLayout);
    createLayout(meshletCullingDescriptorSetLayout, meshletCullingPipelineLayout);
    createLayout(meshletDispatchDescriptorSetLayout, meshletDispatchPipelineLayout);
    createLayout(meshletIndirectDescriptorSetLayout, meshletIndirectPipelineLayout);
    createLayout(grassBuildDescriptorSetLayout, grassBuildPipelineLayout);
    createLayout(grassDispatchBuildDescriptorSetLayout, grassDispatchBuildPipelineLayout);
    createLayout(grassPrefixDescriptorSetLayout, grassPrefixPipelineLayout);
    createLayout(grassScatterDescriptorSetLayout, grassScatterPipelineLayout);
    createLayout(grassFinalizeDescriptorSetLayout, grassFinalizePipelineLayout);
    createLayout(grassPackedCullDescriptorSetLayout, grassPackedCullPipelineLayout);
    createLayout(grassShadowPageCullDescriptorSetLayout, grassShadowPageCullPipelineLayout);
    createLayout(grassBladeCullDescriptorSetLayout, grassBladeCullPipelineLayout);
    createLayout(grassClassifyDescriptorSetLayout, grassClassifyPipelineLayout);
    createLayout(grassPackedBinDescriptorSetLayout, grassPackedBinPipelineLayout);
    createLayout(grassPackedScatterDescriptorSetLayout, grassPackedScatterPipelineLayout);
    createLayout(grassPackedFinalizeDescriptorSetLayout, grassPackedFinalizePipelineLayout);
    createLayout(clusteredLightingDescriptorSetLayout, clusteredLightingPipelineLayout);
    hiZCopyPipeline = createComputePipeline("shaders/hiz_initialize.spv", hiZCopyPipelineLayout);
    hiZReducePipeline = createComputePipeline("shaders/hiz_reduce.spv", hiZReducePipelineLayout);
    vsmPageMarkingPipeline = createComputePipeline("shaders/vsm_page_marking.spv", vsmPageMarkingPipelineLayout);
    vsmPageCompactPipeline = createComputePipeline("shaders/vsm_page_compact.spv", vsmPageCompactPipelineLayout);
    shadowMeshletRefinePipeline =
        createComputePipeline("shaders/shadow_meshlet_refine.spv", shadowMeshletRefinePipelineLayout);
    cullingPipeline = createComputePipeline("shaders/gpu_culling.spv", cullingPipelineLayout);
    instanceCullingPipeline = createComputePipeline("shaders/gpu_instance_culling.spv", instanceCullingPipelineLayout);
    meshletCullingPipeline = createComputePipeline("shaders/meshlet_culling.spv", meshletCullingPipelineLayout);
    meshletDispatchPipeline =
        createComputePipeline("shaders/meshlet_build_dispatch.spv", meshletDispatchPipelineLayout);
    meshletIndirectPipeline = createComputePipeline("shaders/meshlet_build_indexed.spv", meshletIndirectPipelineLayout);
    if (meshletDrawDescriptorSetLayout != VK_NULL_HANDLE) {
        meshletDrawBinPipeline =
            createComputePipeline("shaders/meshlet_build_mesh_draws.spv", meshletIndirectPipelineLayout);
        meshletDrawCommandPipeline =
            createComputePipeline("shaders/meshlet_build_mesh_commands.spv", meshletDispatchPipelineLayout);
        shadowMeshletMeshRefinePipeline =
            createComputePipeline("shaders/shadow_meshlet_refine_mesh.spv", shadowMeshletRefinePipelineLayout);
        shadowMeshletMeshCommandPipeline = createComputePipeline("shaders/shadow_meshlet_build_mesh_commands.spv",
                                                                 shadowMeshletRefinePipelineLayout);
    }
    grassBuildPipeline = createComputePipeline("shaders/grass_build_indirect.spv", grassBuildPipelineLayout);
    grassDispatchBuildPipeline =
        createComputePipeline("shaders/grass_build_dispatch.spv", grassDispatchBuildPipelineLayout);
    grassPrefixPipeline = createComputePipeline("shaders/grass_prefix_sum.spv", grassPrefixPipelineLayout);
    grassScatterPipeline = createComputePipeline("shaders/grass_scatter_instances.spv", grassScatterPipelineLayout);
    grassFinalizePipeline = createComputePipeline("shaders/grass_finalize_indirect.spv", grassFinalizePipelineLayout);
    grassBladeCullPipeline = createComputePipeline("shaders/grass_packed_cull.spv", grassBladeCullPipelineLayout);
    // Cluster cull has the compact 5-binding layout retained above.
    grassPackedCullPipeline = createComputePipeline("shaders/grass_cluster_cull.spv", grassPackedCullPipelineLayout);
    grassShadowPageCullPipeline =
        createComputePipeline("shaders/grass_shadow_page_cull.spv", grassShadowPageCullPipelineLayout);
    grassClassifyPipeline = createComputePipeline("shaders/grass_classify.spv", grassClassifyPipelineLayout);
    grassPackedBinPipeline = createComputePipeline("shaders/grass_packed_bin.spv", grassPackedBinPipelineLayout);
    grassPackedPrefixPipeline = createComputePipeline("shaders/grass_packed_prefix.spv", grassPrefixPipelineLayout);
    grassPackedScatterPipeline =
        createComputePipeline("shaders/grass_packed_scatter.spv", grassPackedScatterPipelineLayout);
    grassPackedFinalizePipeline =
        createComputePipeline("shaders/grass_packed_finalize.spv", grassPackedFinalizePipelineLayout);
    clusteredLightingPipeline =
        createComputePipeline("shaders/clustered_light_culling.spv", clusteredLightingPipelineLayout);

    refreshCullingBatchObjects();
    for (Buffer& buffer : cullingObjectBuffers) {
        buffer.createHostVisible(vulkanDevice.physical(), device, sizeof(Culling::GPUObjectData) * genericCapacity,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vulkanDevice.allocator());
        const Culling::GPUObjectData emptyObject{};
        buffer.update(objectCount == 0 ? &emptyObject : gpuObjects.data(),
                      sizeof(Culling::GPUObjectData) * std::max(1u, objectCount));
    }

    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        const GpuReflectionProbe emptyProbe{};
        reflectionProbeBuffers[frame].createHostVisible(vulkanDevice.physical(), device,
                                                        sizeof(GpuReflectionProbe) * ReflectionProbeManager::MaxProbes,
                                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vulkanDevice.allocator());
        reflectionProbeBuffers[frame].update(&emptyProbe, sizeof(emptyProbe));
        clusteredLightingUniformBuffers[frame].createHostVisible(
            vulkanDevice.physical(), device, sizeof(ClusteredLightingUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            vulkanDevice.allocator());
        localLightBuffers[frame].createHostVisible(vulkanDevice.physical(), device,
                                                   sizeof(LocalLightGPU) * MaxClusteredLocalLights,
                                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vulkanDevice.allocator());
        sceneClusteredLightingUniformBuffers[frame].createHostVisible(
            vulkanDevice.physical(), device, sizeof(ClusteredLightingUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            vulkanDevice.allocator());
        std::vector<ClusterLightRangeGPU> emptyGameRanges(gameClusterCount);
        std::vector<ClusterLightRangeGPU> emptySceneRanges(sceneClusterCount);
        std::vector<std::uint32_t> emptyGameIndices(gameClusterCount * MaxLightsPerCluster);
        std::vector<std::uint32_t> emptySceneIndices(sceneClusterCount * MaxLightsPerCluster);
        clusteredLightRangeBuffers[frame].createDeviceLocal(
            vulkanDevice.physical(), device, emptyGameRanges.data(),
            sizeof(ClusterLightRangeGPU) * emptyGameRanges.size(),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool,
            vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        clusteredLightIndexBuffers[frame].createDeviceLocal(
            vulkanDevice.physical(), device, emptyGameIndices.data(), sizeof(std::uint32_t) * emptyGameIndices.size(),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool,
            vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        sceneClusteredLightRangeBuffers[frame].createDeviceLocal(
            vulkanDevice.physical(), device, emptySceneRanges.data(),
            sizeof(ClusterLightRangeGPU) * emptySceneRanges.size(),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool,
            vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        sceneClusteredLightIndexBuffers[frame].createDeviceLocal(
            vulkanDevice.physical(), device, emptySceneIndices.data(), sizeof(std::uint32_t) * emptySceneIndices.size(),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool,
            vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        cullingUniformBuffers[frame].createHostVisible(vulkanDevice.physical(), device,
                                                       sizeof(Culling::CullingUniformData),
                                                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, vulkanDevice.allocator());
        sceneCullingUniformBuffers[frame].createHostVisible(
            vulkanDevice.physical(), device, sizeof(Culling::CullingUniformData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            vulkanDevice.allocator());
        meshletCullingUniformBuffers[frame].createHostVisible(
            vulkanDevice.physical(), device, sizeof(Culling::MeshletCullUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            vulkanDevice.allocator());
        foliageCullingUniformBuffers[frame].createHostVisible(
            vulkanDevice.physical(), device, sizeof(Culling::CullingUniformData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            vulkanDevice.allocator());
        shadowCullingUniformBuffers[frame].createHostVisible(
            vulkanDevice.physical(), device, sizeof(Culling::CullingUniformData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            vulkanDevice.allocator());
        shadowTwoSidedCullingUniformBuffers[frame].createHostVisible(
            vulkanDevice.physical(), device, sizeof(Culling::CullingUniformData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            vulkanDevice.allocator());
        const std::uint32_t zero = 0;
        std::vector<std::uint32_t> emptyVisibleInstances(
            std::max<std::size_t>(1, sceneGpu.database.instances().size()));
        visibleInstanceBuffers[frame].createDeviceLocal(
            vulkanDevice.physical(), device, emptyVisibleInstances.data(),
            sizeof(std::uint32_t) * emptyVisibleInstances.size(),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool,
            vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        visibleInstanceCountBuffers[frame].createDeviceLocal(
            vulkanDevice.physical(), device, &zero, sizeof(zero),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool,
            vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        const VkDispatchIndirectCommand emptyMeshletCullDispatch{};
        meshletCullDispatchBuffers[frame].createDeviceLocal(
            vulkanDevice.physical(), device, &emptyMeshletCullDispatch, sizeof(emptyMeshletCullDispatch),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        visibleMeshletBuffers[frame].createDeviceLocalEmpty(device, sizeof(Culling::VisibleMeshlet) * meshletCapacity,
                                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                            vulkanDevice.allocator());
        visibleMeshletCountBuffers[frame].createDeviceLocal(
            vulkanDevice.physical(), device, &zero, sizeof(zero),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool,
            vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        meshletIndexedIndirectBuffers[frame].createDeviceLocalEmpty(
            device,
            sizeof(VkDrawIndexedIndirectCommand) *
                (static_cast<VkDeviceSize>(meshletBinBudget) + static_cast<VkDeviceSize>(meshletCapacity)),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, vulkanDevice.allocator());
        // 32 per-slot draw counts (cleared every frame), the packed slot
        // offsets, then the 32 two-sided mesh-shader entry counts (cleared
        // every frame; MeshletDrawTwoSidedCountBase in MeshletDraw.slang).
        std::array<std::uint32_t, MaterialProgramSlotCount * 2 + Culling::DrawBinOffsetCount> emptyMeshletCounts{};
        std::ranges::copy(meshletBinOffsets, emptyMeshletCounts.begin() + MaterialProgramSlotCount);
        meshletIndexedDrawCountBuffers[frame].createDeviceLocal(
            vulkanDevice.physical(), device, emptyMeshletCounts.data(), sizeof(emptyMeshletCounts),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        meshletIndexedIndirectDraws[frame].create(meshletIndexedIndirectBuffers[frame].handle(),
                                                  meshletIndexedDrawCountBuffers[frame].handle(), meshletCapacity);
        if (meshletDrawDescriptorSetLayout != VK_NULL_HANDLE) {
            meshletDrawListBuffers[frame].createDeviceLocalEmpty(
                device, sizeof(std::uint32_t) * 2U * std::max<VkDeviceSize>(1, meshletBinBudget),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vulkanDevice.allocator());
            // All, one-sided and two-sided entries per slot (meshlet_build_mesh_commands).
            const std::array<VkDrawMeshTasksIndirectCommandEXT, MaterialProgramSlotCount * 3> emptyMeshTasks{};
            meshletDrawCommandBuffers[frame].createDeviceLocal(
                vulkanDevice.physical(), device, emptyMeshTasks.data(), sizeof(emptyMeshTasks),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        }
        std::vector<std::uint32_t> emptyGrassBins(grassBinCount, 0U);
        grassBinCountBuffers[frame].createDeviceLocal(
            vulkanDevice.physical(), device, emptyGrassBins.data(), sizeof(std::uint32_t) * emptyGrassBins.size(),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool,
            vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        grassBinOffsetBuffers[frame].createDeviceLocal(
            vulkanDevice.physical(), device, emptyGrassBins.data(), sizeof(std::uint32_t) * emptyGrassBins.size(),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool,
            vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        grassBinCursorBuffers[frame].createDeviceLocal(
            vulkanDevice.physical(), device, emptyGrassBins.data(), sizeof(std::uint32_t) * emptyGrassBins.size(),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool,
            vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        const std::vector<GPUGrassInstance> emptyGeneratedGrass =
            sceneGpu.grassInstances.empty() ? std::vector<GPUGrassInstance>(1) : sceneGpu.grassInstances;
        generatedGrassInstanceBuffers[frame].createDeviceLocal(
            vulkanDevice.physical(), device, emptyGeneratedGrass.data(),
            sizeof(GPUGrassInstance) * emptyGeneratedGrass.size(),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool,
            vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        const std::vector<GPUGrassDeformation> emptyGrassDeformations =
            sceneGpu.grassDeformations.empty() ? std::vector<GPUGrassDeformation>(1) : sceneGpu.grassDeformations;
        // This is the only per-blade buffer updated at runtime. Each
        // frame owns one copy, so mapped coherent memory is safe once
        // that frame's fence has completed and avoids an extra staging
        // submission for every grass interaction.
        grassDeformationBuffers[frame].createHostVisible(vulkanDevice.physical(), device,
                                                         sizeof(GPUGrassDeformation) * emptyGrassDeformations.size(),
                                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vulkanDevice.allocator());
        grassDeformationBuffers[frame].update(emptyGrassDeformations.data(),
                                              sizeof(GPUGrassDeformation) * emptyGrassDeformations.size());
        const std::vector<GPUGrassCluster> emptyGrassClusters =
            sceneGpu.grassClusters.empty() ? std::vector<GPUGrassCluster>(1) : sceneGpu.grassClusters;
        grassClusterBuffers[frame].createDeviceLocal(
            vulkanDevice.physical(), device, emptyGrassClusters.data(),
            sizeof(GPUGrassCluster) * emptyGrassClusters.size(),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool,
            vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        std::vector<VkDrawIndexedIndirectCommand> emptyGrassCommands(grassBinCount);
        grassIndirectBuffers[frame].createDeviceLocal(
            vulkanDevice.physical(), device, emptyGrassCommands.data(),
            sizeof(VkDrawIndexedIndirectCommand) * emptyGrassCommands.size(),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        grassDrawCountBuffers[frame].createDeviceLocal(
            vulkanDevice.physical(), device, &zero, sizeof(zero),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        const std::size_t grassClusterCapacity = std::max<std::size_t>(1, sceneGpu.grassClusters.size());
        const std::size_t grassPageCommandCapacity = grassClusterCapacity * ShadowMap::MaxPageUpdatesPerFrame;
        std::vector<VkDrawIndexedIndirectCommand> emptyGrassPageCommands(grassPageCommandCapacity);
        std::array<std::uint32_t, ShadowMap::MaxPageUpdatesPerFrame> emptyPageCounts{};
        std::array<glm::mat4, ShadowMap::MaxPageUpdatesPerFrame> emptyPageMatrices{};
        grassShadowPageMatricesBuffers[frame].createHostVisible(
            vulkanDevice.physical(), device, sizeof(emptyPageMatrices), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            vulkanDevice.allocator());
        grassShadowPageMatricesBuffers[frame].update(emptyPageMatrices.data(), sizeof(emptyPageMatrices));
        grassShadowPageIndirectBuffers[frame].createDeviceLocal(
            vulkanDevice.physical(), device, emptyGrassPageCommands.data(),
            sizeof(VkDrawIndexedIndirectCommand) * emptyGrassPageCommands.size(),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        grassShadowPageDrawCountBuffers[frame].createDeviceLocal(
            vulkanDevice.physical(), device, emptyPageCounts.data(), sizeof(emptyPageCounts),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        grassShadowPageCullUniformBuffers[frame].createHostVisible(
            vulkanDevice.physical(), device, sizeof(GrassShadowPageCullUniformData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            vulkanDevice.allocator());
        grassIndirectUniformBuffers[frame].createHostVisible(
            vulkanDevice.physical(), device, sizeof(GrassIndirectUniformData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            vulkanDevice.allocator());
        grassPrefixUniformBuffers[frame].createHostVisible(
            vulkanDevice.physical(), device, sizeof(GrassPrefixUniformData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            vulkanDevice.allocator());
        // Binned streams pack every material slot back to back
        // (drawBinOffsets), so all slots together hold at most the object
        // count. The second half keeps offset + maxDrawCount commands
        // inside the buffer for any slot, as vkCmdDrawIndexedIndirectCount
        // requires. Reserving a full object range per slot (x32) made these
        // the largest buffers on dense scenes.
        std::vector<VkDrawIndexedIndirectCommand> emptyCommands(genericCapacity * BinnedIndirectSlack);
        indirectBuffers[frame].createDeviceLocal(
            vulkanDevice.physical(), device, emptyCommands.data(),
            sizeof(VkDrawIndexedIndirectCommand) * genericCapacity * BinnedIndirectSlack,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        auto& lists = grassRenderLists[frame];
        const std::size_t grassCapacity = std::max<std::size_t>(1, sceneGpu.grassInstances.size());
        std::vector<std::uint32_t> emptyVisibleGrass(grassCapacity, 0U);
        std::vector<VkDrawIndexedIndirectCommand> emptyGrassList(
            std::max<std::size_t>(1, sceneGpu.grassClusters.size()));
        lists.visibleInstances.createDeviceLocal(vulkanDevice.physical(), device, emptyVisibleGrass.data(),
                                                 sizeof(std::uint32_t) * grassCapacity,
                                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                 commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        const std::array<std::uint32_t, 3> zeroDispatchCounts{};
        lists.visibleCount.createDeviceLocal(vulkanDevice.physical(), device, zeroDispatchCounts.data(),
                                             sizeof(zeroDispatchCounts),
                                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                             commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        const std::size_t clusterCapacity = std::max<std::size_t>(1, sceneGpu.grassClusters.size());
        std::vector<std::uint32_t> emptyVisibleClusters(clusterCapacity, 0U);
        lists.visibleClusters.createDeviceLocal(vulkanDevice.physical(), device, emptyVisibleClusters.data(),
                                                sizeof(std::uint32_t) * clusterCapacity,
                                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        lists.visibleClusterCount.createDeviceLocal(
            vulkanDevice.physical(), device, &zero, sizeof(zero),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool,
            vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        const auto maxClusterInstances =
            std::max_element(sceneGpu.grassClusters.begin(), sceneGpu.grassClusters.end(),
                             [](const auto& a, const auto& b) { return a.instanceRange.y < b.instanceRange.y; });
        const GrassBladeDispatchData bladeDispatch{maxClusterInstances == sceneGpu.grassClusters.end()
                                                       ? 1U
                                                       : (maxClusterInstances->instanceRange.y + 63U) / 64U,
                                                   0U, 1U};
        lists.bladeCullDispatch.createDeviceLocal(
            vulkanDevice.physical(), device, &bladeDispatch, sizeof(bladeDispatch),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        const std::array<GrassBladeDispatchData, 3> emptyDispatches{};
        lists.dispatchIndirect.createDeviceLocal(
            vulkanDevice.physical(), device, emptyDispatches.data(), sizeof(emptyDispatches),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        for (Buffer* buffer :
             {&lists.mainVisibleInstances, &lists.shadowVisibleInstances, &lists.velocityVisibleInstances})
            buffer->createDeviceLocal(vulkanDevice.physical(), device, emptyVisibleGrass.data(),
                                      sizeof(std::uint32_t) * grassCapacity,
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        std::array<std::uint32_t, 3> zeroStreamCounts{};
        lists.classifyCounts.createDeviceLocal(vulkanDevice.physical(), device, zeroStreamCounts.data(),
                                               sizeof(zeroStreamCounts),
                                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                               commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        std::vector<std::uint32_t> zeroBins(clusterCapacity, 0U);
        for (uint32_t stream = 0; stream < 3; ++stream) {
            for (Buffer* buffer : {&lists.binCounts[stream], &lists.binOffsets[stream], &lists.binCursors[stream]})
                buffer->createDeviceLocal(vulkanDevice.physical(), device, zeroBins.data(),
                                          sizeof(std::uint32_t) * clusterCapacity,
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                          commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
            lists.drawInstances[stream].createDeviceLocal(
                vulkanDevice.physical(), device, emptyVisibleGrass.data(), sizeof(std::uint32_t) * grassCapacity,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool,
                vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        }
        grassClassifyUniformBuffers[frame].createHostVisible(
            vulkanDevice.physical(), device, sizeof(GrassClassifyUniformData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            vulkanDevice.allocator());
        grassPackedCullUniformBuffers[frame].createHostVisible(
            vulkanDevice.physical(), device, sizeof(GrassPackedCullUniformData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            vulkanDevice.allocator());
        for (Buffer& buffer : grassPackedStreamUniformBuffers[frame])
            buffer.createHostVisible(vulkanDevice.physical(), device, sizeof(GrassPackedStreamUniformData),
                                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, vulkanDevice.allocator());
        for (Buffer* buffer : {&lists.mainIndirect, &lists.shadowIndirect, &lists.velocityIndirect})
            buffer->createDeviceLocal(vulkanDevice.physical(), device, emptyGrassList.data(),
                                      sizeof(VkDrawIndexedIndirectCommand) * emptyGrassList.size(),
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        for (Buffer* buffer : {&lists.mainDrawCount, &lists.shadowDrawCount, &lists.velocityDrawCount})
            buffer->createDeviceLocal(vulkanDevice.physical(), device, &zero, sizeof(zero),
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        // Scene descriptors share the grass scratch buffers created
        // above for Game View; only descriptor-set identities differ.
        foliageIndirectBuffers[frame].createDeviceLocal(
            vulkanDevice.physical(), device, emptyCommands.data(),
            sizeof(VkDrawIndexedIndirectCommand) * genericCapacity * BinnedIndirectSlack,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        // Candidates are strided by each pass's maxDrawCount (its category capacity).
        std::vector<std::uint32_t> emptyShadowCandidates(
            std::max(shadowPassCapacity, shadowTwoSidedPassCapacity) * ShadowMap::ClipLevelCount);
        shadowCandidateBuffers[frame].createDeviceLocal(
            vulkanDevice.physical(), device, emptyShadowCandidates.data(),
            sizeof(std::uint32_t) * shadowPassCapacity * ShadowMap::ClipLevelCount,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool,
            vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        shadowTwoSidedCandidateBuffers[frame].createDeviceLocal(
            vulkanDevice.physical(), device, emptyShadowCandidates.data(),
            sizeof(std::uint32_t) * shadowTwoSidedPassCapacity * ShadowMap::ClipLevelCount,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool,
            vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        std::array<std::uint32_t, ShadowMap::ClipLevelCount> zeroCandidateCounts{};
        shadowCandidateCountBuffers[frame].createDeviceLocal(
            vulkanDevice.physical(), device, zeroCandidateCounts.data(), sizeof(zeroCandidateCounts),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool,
            vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        shadowTwoSidedCandidateCountBuffers[frame].createDeviceLocal(
            vulkanDevice.physical(), device, zeroCandidateCounts.data(), sizeof(zeroCandidateCounts),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool,
            vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        std::array<VkDispatchIndirectCommand, ShadowMap::ClipLevelCount> emptyCandidateDispatches{};
        shadowCandidateDispatchBuffers[frame].createDeviceLocal(
            vulkanDevice.physical(), device, emptyCandidateDispatches.data(), sizeof(emptyCandidateDispatches),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        shadowTwoSidedCandidateDispatchBuffers[frame].createDeviceLocal(
            vulkanDevice.physical(), device, emptyCandidateDispatches.data(), sizeof(emptyCandidateDispatches),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        shadowIndirectBuffers[frame].createDeviceLocalEmpty(
            device, sizeof(VkDrawIndexedIndirectCommand) * shadowPassCapacity * ShadowMap::MaxPageUpdatesPerFrame,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, vulkanDevice.allocator());
        // Meshlet refinement: one object list and one bounded meshlet draw
        // list per page; the counters are cleared before every page cull.
        // The one-sided [0] and two-sided [1] streams keep separate lists.
        const auto createMeshletLists = [&](Buffer& objects, Buffer& objectCounts, Buffer& commands,
                                            Buffer& taskCommands, Buffer& commandCounts,
                                            const std::uint32_t passCapacity) {
            objects.createDeviceLocalEmpty(
                device, sizeof(std::uint32_t) * 4 * passCapacity * ShadowMap::MaxPageUpdatesPerFrame,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vulkanDevice.allocator());
            objectCounts.createDeviceLocalEmpty(
                device, sizeof(std::uint32_t) * ShadowMap::MaxPageUpdatesPerFrame,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, vulkanDevice.allocator());
            commands.createDeviceLocalEmpty(device,
                                            sizeof(VkDrawIndexedIndirectCommand) * ShadowPass::MeshletDrawsPerPage *
                                                ShadowMap::MaxPageUpdatesPerFrame,
                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                            vulkanDevice.allocator());
            taskCommands.createDeviceLocalEmpty(
                device, sizeof(VkDrawMeshTasksIndirectCommandEXT) * ShadowMap::MaxPageUpdatesPerFrame,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, vulkanDevice.allocator());
            commandCounts.createDeviceLocalEmpty(
                device, sizeof(std::uint32_t) * ShadowMap::MaxPageUpdatesPerFrame,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                vulkanDevice.allocator());
        };
        createMeshletLists(shadowMeshletObjectBuffers[frame], shadowMeshletObjectCountBuffers[frame],
                           shadowMeshletCommandBuffers[frame], shadowMeshletTaskCommandBuffers[frame],
                           shadowMeshletCommandCountBuffers[frame], shadowPassCapacity);
        createMeshletLists(shadowTwoSidedMeshletObjectBuffers[frame], shadowTwoSidedMeshletObjectCountBuffers[frame],
                           shadowTwoSidedMeshletCommandBuffers[frame], shadowTwoSidedMeshletTaskCommandBuffers[frame],
                           shadowTwoSidedMeshletCommandCountBuffers[frame], shadowTwoSidedPassCapacity);
        // Page entries, then one candidate-cull entry per clip level.
        std::array<Culling::ShadowPageWork, ShadowMap::MaxPageUpdatesPerFrame + ShadowMap::ClipLevelCount>
            emptyPageWork{};
        shadowPageWorkBuffers[frame].createHostVisible(vulkanDevice.physical(), device, sizeof(emptyPageWork),
                                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vulkanDevice.allocator());
        constexpr std::uint32_t vsmRequestWordCount = (ShadowMap::VirtualPageCount + 31U) / 32U;
        // Only the GPU touches the request bitset (marking, then compaction):
        // in host-visible memory every marking atomic would cross PCIe.
        vsmRequestedPageBuffers[frame].createDeviceLocalEmpty(
            device, sizeof(std::uint32_t) * vsmRequestWordCount,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, vulkanDevice.allocator());
        const std::array<std::uint32_t, ShadowMap::VirtualPageCount> emptyVsmPageKeys{};
        const std::uint32_t emptyVsmPageCount = 0;
        vsmCompactedPageBuffers[frame].createHostVisible(vulkanDevice.physical(), device, sizeof(emptyVsmPageKeys),
                                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vulkanDevice.allocator());
        vsmCompactedPageCountBuffers[frame].createHostVisible(
            vulkanDevice.physical(), device, sizeof(emptyVsmPageCount),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, vulkanDevice.allocator());
        vsmPageMarkingUniformBuffers[frame].createHostVisible(
            vulkanDevice.physical(), device, sizeof(VsmPageMarkingUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            vulkanDevice.allocator());
        shadowTwoSidedIndirectBuffers[frame].createDeviceLocalEmpty(
            device,
            sizeof(VkDrawIndexedIndirectCommand) * shadowTwoSidedPassCapacity * ShadowMap::MaxPageUpdatesPerFrame,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, vulkanDevice.allocator());
        std::array<std::uint32_t, ShadowMap::MaxPageUpdatesPerFrame> zeroShadowCounts{};
        // One independent compact range per page lets the 2D page
        // culling dispatch write transforms without cross-page atomics.
        const std::array<std::size_t, 2> shadowTransformCounts{
            std::size_t{shadowPageInstanceCapacity[0]} * ShadowMap::MaxPageUpdatesPerFrame,
            std::size_t{shadowPageInstanceCapacity[1]} * ShadowMap::MaxPageUpdatesPerFrame};
        shadowInstanceTransformBuffers[frame].createDeviceLocalEmpty(
            device, sizeof(RendererInstanceData) * shadowTransformCounts[0], VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            vulkanDevice.allocator());
        shadowTwoSidedInstanceTransformBuffers[frame].createDeviceLocalEmpty(
            device, sizeof(RendererInstanceData) * shadowTransformCounts[1], VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            vulkanDevice.allocator());
        shadowVisibleInstanceBuffers[frame].createDeviceLocalEmpty(
            device, sizeof(Culling::ShadowVisibleInstance) * shadowTransformCounts[0],
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vulkanDevice.allocator());
        shadowTwoSidedVisibleInstanceBuffers[frame].createDeviceLocalEmpty(
            device, sizeof(Culling::ShadowVisibleInstance) * shadowTransformCounts[1],
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vulkanDevice.allocator());
        shadowInstanceTransformCountBuffers[frame].createDeviceLocal(
            vulkanDevice.physical(), device, zeroShadowCounts.data(), sizeof(zeroShadowCounts),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool,
            vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        shadowTwoSidedInstanceTransformCountBuffers[frame].createDeviceLocal(
            vulkanDevice.physical(), device, zeroShadowCounts.data(), sizeof(zeroShadowCounts),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, commandPool,
            vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        shadowInstanceMaterialBuffers[frame].createDeviceLocalEmpty(
            device, sizeof(std::uint32_t) * shadowTransformCounts[0], VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            vulkanDevice.allocator());
        shadowTwoSidedInstanceMaterialBuffers[frame].createDeviceLocalEmpty(
            device, sizeof(std::uint32_t) * shadowTransformCounts[1], VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            vulkanDevice.allocator());
        const std::array<std::uint32_t, MaterialProgramSlotCount> zeroMaterialDrawCounts{};
        drawCountBuffers[frame].createDeviceLocal(
            vulkanDevice.physical(), device, zeroMaterialDrawCounts.data(), sizeof(zeroMaterialDrawCounts),
            // Transfer source: GlassPass copies its slot's count out.
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        foliageDrawCountBuffers[frame].createDeviceLocal(
            vulkanDevice.physical(), device, zeroMaterialDrawCounts.data(), sizeof(zeroMaterialDrawCounts),
            // Transfer source: GlassPass copies its slot's count out.
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        shadowDrawCountBuffers[frame].createDeviceLocal(
            vulkanDevice.physical(), device, zeroShadowCounts.data(), sizeof(zeroShadowCounts),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
        shadowTwoSidedDrawCountBuffers[frame].createDeviceLocal(
            vulkanDevice.physical(), device, zeroShadowCounts.data(), sizeof(zeroShadowCounts),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            commandPool, vulkanDevice.graphicsQueue(), vulkanDevice.allocator());
    }

    constexpr uint32_t cullingSetCount = MAX_FRAMES_IN_FLIGHT * 6;
    constexpr uint32_t instanceCullSetCount = MAX_FRAMES_IN_FLIGHT;
    // Per frame: 4 legacy sets, game cluster/blade/classify sets,
    // 14 per-frame shared sets (including four dispatch builders),
    // plus 24 stream-builder sets and one VSM page-cull set.
    // Keep this in lockstep with allocateGrassSets below.
    constexpr uint32_t grassSetCount = MAX_FRAMES_IN_FLIGHT * 39;
    const uint32_t hiZDescriptorSetCount = hiZBuffers[0].mipCount() * MAX_FRAMES_IN_FLIGHT;
    const uint32_t imageDescriptors = hiZDescriptorSetCount + cullingSetCount;
    const VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, imageDescriptors + MAX_FRAMES_IN_FLIGHT},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         // The trailing 15: mesh-shader bin (7), command (2) and draw (6) sets;
         // then the VSM mesh-task commands (1) and caster draw set (6), and
         // the two-sided stream's refine (10) and caster draw (6) sets.
         cullingSetCount * 14 + instanceCullSetCount * 3 + MAX_FRAMES_IN_FLIGHT * (152 + 9 + 2 + 15 + 7 + 16)},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, cullingSetCount + MAX_FRAMES_IN_FLIGHT * 31},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, hiZDescriptorSetCount},
    };
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets =
        hiZDescriptorSetCount + cullingSetCount + instanceCullSetCount + grassSetCount + MAX_FRAMES_IN_FLIGHT * 14;
    poolInfo.poolSizeCount = allocateHiZ ? std::size(poolSizes) : std::size(poolSizes) - 1;
    poolInfo.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &cullingDescriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("Could not create Hi-Z descriptor pool");
    }
    if (allocateHiZ) {
        for (std::uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
            hiZPasses[frame].create(device, cullingDescriptorPool, hiZCopyPipeline, hiZCopyPipelineLayout,
                                    hiZCopyDescriptorSetLayout, hiZReducePipeline, hiZReducePipelineLayout,
                                    hiZReduceDescriptorSetLayout, hiZBuffers[frame],
                                    msaa.enabled() ? hiZDepthBuffer.imageView() : depthBuffer.imageView(),
                                    msaa.enabled() ? hiZDepthBuffer.sampler() : depthBuffer.sampler());
        }
    }

    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT * 6> cullLayouts{};
    cullLayouts.fill(cullingDescriptorSetLayout);
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT * 6> cullSets{};
    VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocateInfo.descriptorPool = cullingDescriptorPool;
    allocateInfo.descriptorSetCount = static_cast<uint32_t>(cullSets.size());
    allocateInfo.pSetLayouts = cullLayouts.data();
    if (vkAllocateDescriptorSets(device, &allocateInfo, cullSets.data()) != VK_SUCCESS) {
        throw std::runtime_error("Could not allocate culling descriptor sets");
    }
    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> vsmMarkingLayouts{};
    vsmMarkingLayouts.fill(vsmPageMarkingDescriptorSetLayout);
    allocateInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    allocateInfo.pSetLayouts = vsmMarkingLayouts.data();
    if (vkAllocateDescriptorSets(device, &allocateInfo, vsmPageMarkingSets.data()) != VK_SUCCESS) {
        throw std::runtime_error("Could not allocate VSM page-marking descriptor sets");
    }
    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> vsmCompactLayouts{};
    vsmCompactLayouts.fill(vsmPageCompactDescriptorSetLayout);
    allocateInfo.pSetLayouts = vsmCompactLayouts.data();
    if (vkAllocateDescriptorSets(device, &allocateInfo, vsmPageCompactSets.data()) != VK_SUCCESS) {
        throw std::runtime_error("Could not allocate VSM page-compaction descriptor sets");
    }
    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> refineLayouts{};
    refineLayouts.fill(shadowMeshletRefineDescriptorSetLayout);
    allocateInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    allocateInfo.pSetLayouts = refineLayouts.data();
    if (vkAllocateDescriptorSets(device, &allocateInfo, shadowMeshletRefineSets.data()) != VK_SUCCESS ||
        vkAllocateDescriptorSets(device, &allocateInfo, shadowTwoSidedMeshletRefineSets.data()) != VK_SUCCESS)
        throw std::runtime_error("Could not allocate VSM meshlet-refine descriptor sets");
    shadowMeshletPassCapacity = shadowPassCapacity;
    shadowTwoSidedMeshletPassCapacity = shadowTwoSidedPassCapacity;
    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> instanceCullLayouts{};
    instanceCullLayouts.fill(instanceCullingDescriptorSetLayout);
    allocateInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    allocateInfo.pSetLayouts = instanceCullLayouts.data();
    if (vkAllocateDescriptorSets(device, &allocateInfo, instanceCullSets.data()) != VK_SUCCESS) {
        throw std::runtime_error("Could not allocate instance-culling descriptor sets");
    }
    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> meshletCullLayouts{};
    meshletCullLayouts.fill(meshletCullingDescriptorSetLayout);
    allocateInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    allocateInfo.pSetLayouts = meshletCullLayouts.data();
    if (vkAllocateDescriptorSets(device, &allocateInfo, meshletCullSets.data()) != VK_SUCCESS) {
        throw std::runtime_error("Could not allocate meshlet-culling descriptor sets");
    }
    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> meshletDispatchLayouts{};
    meshletDispatchLayouts.fill(meshletDispatchDescriptorSetLayout);
    allocateInfo.pSetLayouts = meshletDispatchLayouts.data();
    if (vkAllocateDescriptorSets(device, &allocateInfo, meshletDispatchSets.data()) != VK_SUCCESS) {
        throw std::runtime_error("Could not allocate meshlet-dispatch descriptor sets");
    }
    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> meshletIndirectLayouts{};
    meshletIndirectLayouts.fill(meshletIndirectDescriptorSetLayout);
    allocateInfo.pSetLayouts = meshletIndirectLayouts.data();
    if (vkAllocateDescriptorSets(device, &allocateInfo, meshletIndirectSets.data()) != VK_SUCCESS) {
        throw std::runtime_error("Could not allocate meshlet-indirect descriptor sets");
    }
    if (meshletDrawDescriptorSetLayout != VK_NULL_HANDLE) {
        std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> meshletDrawLayouts{};
        meshletDrawLayouts.fill(meshletDrawDescriptorSetLayout);
        const auto allocateMeshletDrawSets = [&](const auto& layouts, auto& sets) {
            allocateInfo.pSetLayouts = layouts.data();
            if (vkAllocateDescriptorSets(device, &allocateInfo, sets.data()) != VK_SUCCESS)
                throw std::runtime_error("Could not allocate mesh-shader meshlet descriptor sets");
        };
        allocateMeshletDrawSets(meshletIndirectLayouts, meshletDrawBinSets);
        allocateMeshletDrawSets(meshletDispatchLayouts, meshletDrawCommandSets);
        allocateMeshletDrawSets(meshletDrawLayouts, meshletDrawSets);
        allocateMeshletDrawSets(meshletDrawLayouts, shadowMeshletDrawSets);
        allocateMeshletDrawSets(meshletDrawLayouts, shadowTwoSidedMeshletDrawSets);
    }
    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT * 2> clusteredLayouts{};
    clusteredLayouts.fill(clusteredLightingDescriptorSetLayout);
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT * 2> clusteredSets{};
    allocateInfo.descriptorSetCount = static_cast<uint32_t>(clusteredSets.size());
    allocateInfo.pSetLayouts = clusteredLayouts.data();
    if (vkAllocateDescriptorSets(device, &allocateInfo, clusteredSets.data()) != VK_SUCCESS)
        throw std::runtime_error("Could not allocate clustered-light descriptor sets");
    std::copy_n(clusteredSets.begin(), MAX_FRAMES_IN_FLIGHT, clusteredLightingSets.begin());
    std::copy_n(clusteredSets.begin() + MAX_FRAMES_IN_FLIGHT, MAX_FRAMES_IN_FLIGHT, sceneClusteredLightingSets.begin());
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        const auto updateClusterSet = [&](VkDescriptorSet set, VkBuffer frameUniform, VkBuffer ranges, VkBuffer indices,
                                          VkBuffer clusteredUniform, VkBuffer probes) {
            const VkDescriptorBufferInfo infos[] = {{frameUniform, 0, sizeof(UniformBufferObject)},
                                                    {ranges, 0, VK_WHOLE_SIZE},
                                                    {indices, 0, VK_WHOLE_SIZE},
                                                    {clusteredUniform, 0, sizeof(ClusteredLightingUniforms)},
                                                    {probes, 0, VK_WHOLE_SIZE},
                                                    {localLightBuffers[frame].handle(), 0, VK_WHOLE_SIZE}};
            VkWriteDescriptorSet writes[6]{};
            for (uint32_t binding = 0; binding < 6; ++binding) {
                writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                   nullptr,
                                   set,
                                   binding,
                                   0,
                                   1,
                                   binding == 0 || binding == 3 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                                : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   nullptr,
                                   &infos[binding],
                                   nullptr};
            }
            vkUpdateDescriptorSets(device, 6, writes, 0, nullptr);
        };
        updateClusterSet(clusteredLightingSets[frame], uniformBuffers[frame].handle(),
                         clusteredLightRangeBuffers[frame].handle(), clusteredLightIndexBuffers[frame].handle(),
                         clusteredLightingUniformBuffers[frame].handle(), reflectionProbeBuffers[frame].handle());
        updateClusterSet(sceneClusteredLightingSets[frame], sceneUniformBuffers[frame].handle(),
                         sceneClusteredLightRangeBuffers[frame].handle(),
                         sceneClusteredLightIndexBuffers[frame].handle(),
                         sceneClusteredLightingUniformBuffers[frame].handle(), reflectionProbeBuffers[frame].handle());
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
    allocateGrassSets(grassShadowPageCullDescriptorSetLayout, grassShadowPageCullSets);
    allocateGrassSets(grassBladeCullDescriptorSetLayout, grassBladeCullSets);
    allocateGrassSets(grassClassifyDescriptorSetLayout, grassClassifySets);
    allocateGrassSets(grassPackedCullDescriptorSetLayout, sceneGrassPackedCullSets);
    allocateGrassSets(grassBladeCullDescriptorSetLayout, sceneGrassBladeCullSets);
    allocateGrassSets(grassClassifyDescriptorSetLayout, sceneGrassClassifySets);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        for (uint32_t stream = 0; stream < 3; ++stream) {
            const auto allocateOne = [&](VkDescriptorSetLayout layout, VkDescriptorSet& set) {
                allocateInfo.descriptorSetCount = 1;
                allocateInfo.pSetLayouts = &layout;
                if (vkAllocateDescriptorSets(device, &allocateInfo, &set) != VK_SUCCESS)
                    throw std::runtime_error("Could not allocate packed grass descriptor set");
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
        const VkDescriptorBufferInfo sceneInstanceInfo{gpuVisibilityInstanceBuffers[frame].handle(), 0, VK_WHOLE_SIZE};
        const VkDescriptorBufferInfo visibleInfo{visibleInstanceBuffers[frame].handle(), 0, VK_WHOLE_SIZE};
        const VkDescriptorBufferInfo visibleCountInfo{visibleInstanceCountBuffers[frame].handle(), 0, VK_WHOLE_SIZE};
        const VkDescriptorBufferInfo instanceUniformInfo{cullingUniformBuffers[frame].handle(), 0,
                                                         sizeof(Culling::CullingUniformData)};
        std::array<VkWriteDescriptorSet, 4> instanceWrites{};
        const std::array<const VkDescriptorBufferInfo*, 4> instanceInfos{&sceneInstanceInfo, &visibleInfo,
                                                                         &visibleCountInfo, &instanceUniformInfo};
        for (std::uint32_t binding = 0; binding < instanceWrites.size(); ++binding) {
            instanceWrites[binding] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = instanceCullSets[frame],
                                       .dstBinding = binding,
                                       .descriptorCount = 1,
                                       .descriptorType = binding == 3 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                                      : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       .pBufferInfo = instanceInfos[binding]};
        }
        vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(instanceWrites.size()), instanceWrites.data(), 0,
                               nullptr);
        const auto updateGrassSet = [&](const VkDescriptorSet set, std::initializer_list<VkBuffer> storageBuffers,
                                        const VkBuffer uniformBuffer) {
            std::vector<VkDescriptorBufferInfo> infos;
            infos.reserve(storageBuffers.size() + (uniformBuffer != VK_NULL_HANDLE ? 1 : 0));
            for (const VkBuffer buffer : storageBuffers)
                infos.push_back({buffer, 0, VK_WHOLE_SIZE});
            if (uniformBuffer != VK_NULL_HANDLE)
                infos.push_back({uniformBuffer, 0, VK_WHOLE_SIZE});
            std::vector<VkWriteDescriptorSet> writes(infos.size());
            for (std::uint32_t binding = 0; binding < writes.size(); ++binding) {
                writes[binding] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                   .dstSet = set,
                                   .dstBinding = binding,
                                   .descriptorCount = 1,
                                   .descriptorType = uniformBuffer != VK_NULL_HANDLE && binding + 1 == writes.size()
                                                         ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                         : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   .pBufferInfo = &infos[binding]};
            }
            vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        };
        auto& packedLists = grassRenderLists[frame];
        updateGrassSet(grassVisibleDispatchBuildSets[frame],
                       {packedLists.visibleCount.handle(), packedLists.dispatchIndirect.handle()}, VK_NULL_HANDLE);
        updateGrassSet(grassStreamDispatchBuildSets[frame],
                       {packedLists.classifyCounts.handle(), packedLists.dispatchIndirect.handle()}, VK_NULL_HANDLE);
        updateGrassSet(grassPackedCullSets[frame],
                       {grassClusterBuffers[frame].handle(), packedLists.visibleClusters.handle(),
                        packedLists.visibleClusterCount.handle(), packedLists.bladeCullDispatch.handle()},
                       grassPackedCullUniformBuffers[frame].handle());
        updateGrassSet(grassShadowPageCullSets[frame],
                       {grassClusterBuffers[frame].handle(), packedLists.shadowIndirect.handle(),
                        grassShadowPageMatricesBuffers[frame].handle(), grassShadowPageIndirectBuffers[frame].handle(),
                        grassShadowPageDrawCountBuffers[frame].handle()},
                       grassShadowPageCullUniformBuffers[frame].handle());
        updateGrassSet(grassBladeCullSets[frame],
                       {generatedGrassInstanceBuffers[frame].handle(), grassClusterBuffers[frame].handle(),
                        packedLists.visibleClusters.handle(), packedLists.visibleClusterCount.handle(),
                        packedLists.visibleInstances.handle(), packedLists.visibleCount.handle()},
                       grassPackedCullUniformBuffers[frame].handle());
        updateGrassSet(grassClassifySets[frame],
                       {generatedGrassInstanceBuffers[frame].handle(), grassClusterBuffers[frame].handle(),
                        packedLists.visibleInstances.handle(), packedLists.mainVisibleInstances.handle(),
                        packedLists.shadowVisibleInstances.handle(), packedLists.velocityVisibleInstances.handle(),
                        packedLists.classifyCounts.handle(), packedLists.visibleCount.handle()},
                       grassClassifyUniformBuffers[frame].handle());
        const std::array<VkBuffer, 3> classified{packedLists.mainVisibleInstances.handle(),
                                                 packedLists.shadowVisibleInstances.handle(),
                                                 packedLists.velocityVisibleInstances.handle()};
        const std::array<VkBuffer, 3> indirect{packedLists.mainIndirect.handle(), packedLists.shadowIndirect.handle(),
                                               packedLists.velocityIndirect.handle()};
        const std::array<VkBuffer, 3> drawCounts{packedLists.mainDrawCount.handle(),
                                                 packedLists.shadowDrawCount.handle(),
                                                 packedLists.velocityDrawCount.handle()};
        for (uint32_t stream = 0; stream < 3; ++stream) {
            const VkBuffer uniform = grassPackedStreamUniformBuffers[frame][stream].handle();
            updateGrassSet(grassPackedBinSets[frame][stream],
                           {generatedGrassInstanceBuffers[frame].handle(), classified[stream],
                            packedLists.classifyCounts.handle(), packedLists.binCounts[stream].handle()},
                           uniform);
            updateGrassSet(grassPackedPrefixSets[frame][stream],
                           {packedLists.binCounts[stream].handle(), packedLists.binOffsets[stream].handle()}, uniform);
            updateGrassSet(grassPackedScatterSets[frame][stream],
                           {generatedGrassInstanceBuffers[frame].handle(), classified[stream],
                            packedLists.classifyCounts.handle(), packedLists.binOffsets[stream].handle(),
                            packedLists.binCursors[stream].handle(), packedLists.drawInstances[stream].handle()},
                           uniform);
            updateGrassSet(grassPackedFinalizeSets[frame][stream],
                           {grassClusterBuffers[frame].handle(), packedLists.binCounts[stream].handle(),
                            packedLists.binOffsets[stream].handle(), indirect[stream], drawCounts[stream]},
                           uniform);
        }
        auto& scenePackedLists = sceneGrassRenderLists[frame];
        updateGrassSet(sceneGrassVisibleDispatchBuildSets[frame],
                       {scenePackedLists.visibleCount.handle(), scenePackedLists.dispatchIndirect.handle()},
                       VK_NULL_HANDLE);
        updateGrassSet(sceneGrassStreamDispatchBuildSets[frame],
                       {scenePackedLists.classifyCounts.handle(), scenePackedLists.dispatchIndirect.handle()},
                       VK_NULL_HANDLE);
        updateGrassSet(sceneGrassPackedCullSets[frame],
                       {grassClusterBuffers[frame].handle(), scenePackedLists.visibleClusters.handle(),
                        scenePackedLists.visibleClusterCount.handle(), scenePackedLists.bladeCullDispatch.handle()},
                       sceneGrassPackedCullUniformBuffers[frame].handle());
        updateGrassSet(sceneGrassBladeCullSets[frame],
                       {generatedGrassInstanceBuffers[frame].handle(), grassClusterBuffers[frame].handle(),
                        scenePackedLists.visibleClusters.handle(), scenePackedLists.visibleClusterCount.handle(),
                        scenePackedLists.visibleInstances.handle(), scenePackedLists.visibleCount.handle()},
                       sceneGrassPackedCullUniformBuffers[frame].handle());
        const VkBuffer sceneUniform = sceneGrassPackedStreamUniformBuffers[frame][0].handle();
        updateGrassSet(sceneGrassClassifySets[frame],
                       {generatedGrassInstanceBuffers[frame].handle(), grassClusterBuffers[frame].handle(),
                        scenePackedLists.visibleInstances.handle(), scenePackedLists.mainVisibleInstances.handle(),
                        scenePackedLists.mainVisibleInstances.handle(), scenePackedLists.mainVisibleInstances.handle(),
                        scenePackedLists.classifyCounts.handle(), scenePackedLists.visibleCount.handle()},
                       sceneGrassClassifyUniformBuffers[frame].handle());
        updateGrassSet(sceneGrassPackedBinSets[frame][0],
                       {generatedGrassInstanceBuffers[frame].handle(), scenePackedLists.mainVisibleInstances.handle(),
                        scenePackedLists.classifyCounts.handle(), scenePackedLists.binCounts[0].handle()},
                       sceneUniform);
        updateGrassSet(sceneGrassPackedPrefixSets[frame][0],
                       {scenePackedLists.binCounts[0].handle(), scenePackedLists.binOffsets[0].handle()}, sceneUniform);
        updateGrassSet(sceneGrassPackedScatterSets[frame][0],
                       {generatedGrassInstanceBuffers[frame].handle(), scenePackedLists.mainVisibleInstances.handle(),
                        scenePackedLists.classifyCounts.handle(), scenePackedLists.binOffsets[0].handle(),
                        scenePackedLists.binCursors[0].handle(), scenePackedLists.drawInstances[0].handle()},
                       sceneUniform);
        updateGrassSet(sceneGrassPackedFinalizeSets[frame][0],
                       {grassClusterBuffers[frame].handle(), scenePackedLists.binCounts[0].handle(),
                        scenePackedLists.binOffsets[0].handle(), scenePackedLists.mainIndirect.handle(),
                        scenePackedLists.mainDrawCount.handle()},
                       sceneUniform);
        updateGrassSet(grassBuildSets[frame],
                       {gpuSceneInstanceBuffers[frame].handle(), visibleInstanceBuffers[frame].handle(),
                        visibleInstanceCountBuffers[frame].handle(), gpuSceneMeshBuffers[frame].handle(),
                        grassBinCountBuffers[frame].handle()},
                       grassIndirectUniformBuffers[frame].handle());
        updateGrassSet(grassPrefixSets[frame],
                       {grassBinCountBuffers[frame].handle(), grassBinOffsetBuffers[frame].handle()},
                       grassPrefixUniformBuffers[frame].handle());
        updateGrassSet(grassScatterSets[frame],
                       {gpuSceneInstanceBuffers[frame].handle(), visibleInstanceBuffers[frame].handle(),
                        visibleInstanceCountBuffers[frame].handle(), grassBinOffsetBuffers[frame].handle(),
                        grassBinCursorBuffers[frame].handle(), compactGrassInstanceBuffers[frame].handle()},
                       grassIndirectUniformBuffers[frame].handle());
        updateGrassSet(grassFinalizeSets[frame],
                       {gpuSceneMeshBuffers[frame].handle(), grassBinCountBuffers[frame].handle(),
                        grassBinOffsetBuffers[frame].handle(), grassIndirectBuffers[frame].handle(),
                        grassDrawCountBuffers[frame].handle()},
                       grassIndirectUniformBuffers[frame].handle());
        // Binding 3 is retained by the shared culling pipeline.  When
        // Hi-Z is disabled the shader's feature flag prevents a read,
        // so HDR is a small, already-allocated valid fallback.
        const auto previousHiZFrame = (frame + MAX_FRAMES_IN_FLIGHT - 1U) % MAX_FRAMES_IN_FLIGHT;
        const auto& hiZBuffer = hiZBuffers[previousHiZFrame];
        const VkDescriptorImageInfo hiZInfo = hiZBuffer.image() != VK_NULL_HANDLE
                                                  ? VkDescriptorImageInfo{hiZBuffer.sampler(), hiZBuffer.fullView(),
                                                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
                                                  : VkDescriptorImageInfo{hdrBuffer.sampler(), hdrBuffer.imageView(),
                                                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorBufferInfo vsmRequestInfo{vsmRequestedPageBuffers[frame].handle(), 0, VK_WHOLE_SIZE};
        const VkDescriptorBufferInfo vsmUniformInfo{vsmPageMarkingUniformBuffers[frame].handle(), 0,
                                                    sizeof(VsmPageMarkingUniforms)};
        const VkWriteDescriptorSet vsmWrites[] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = vsmPageMarkingSets[frame],
             .dstBinding = 0,
             .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .pImageInfo = &hiZInfo},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = vsmPageMarkingSets[frame],
             .dstBinding = 1,
             .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             .pBufferInfo = &vsmRequestInfo},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = vsmPageMarkingSets[frame],
             .dstBinding = 2,
             .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
             .pBufferInfo = &vsmUniformInfo},
        };
        vkUpdateDescriptorSets(device, std::size(vsmWrites), vsmWrites, 0, nullptr);
        const VkDescriptorBufferInfo vsmCompactInfos[] = {
            {vsmRequestedPageBuffers[frame].handle(), 0, VK_WHOLE_SIZE},
            {vsmCompactedPageBuffers[frame].handle(), 0, VK_WHOLE_SIZE},
            {vsmCompactedPageCountBuffers[frame].handle(), 0, sizeof(std::uint32_t)},
        };
        VkWriteDescriptorSet vsmCompactWrites[3]{};
        for (std::uint32_t binding = 0; binding < std::size(vsmCompactWrites); ++binding) {
            vsmCompactWrites[binding] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstSet = vsmPageCompactSets[frame],
                                         .dstBinding = binding,
                                         .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                         .pBufferInfo = &vsmCompactInfos[binding]};
        }
        vkUpdateDescriptorSets(device, std::size(vsmCompactWrites), vsmCompactWrites, 0, nullptr);
        struct CullingSetUpdate {
            VkDescriptorSet set;
            const Buffer& indirectBuffer;
            const Buffer& countBuffer;
            const Buffer& uniformBuffer;
            const Buffer& candidateBuffer;
            const Buffer& candidateCountBuffer;
            const Buffer& candidateDispatchBuffer;
            const Buffer& pageWorkBuffer;
            const Buffer& shadowTransformBuffer;
            const Buffer& shadowTransformCountBuffer;
            const Buffer& shadowMaterialBuffer;
            const Buffer& shadowVisibleInstanceBuffer;
            const Buffer& meshletObjectBuffer;
            const Buffer& meshletObjectCountBuffer;
        };
        const auto updateCullingSet = [&](const CullingSetUpdate& update) {
            const VkDescriptorBufferInfo objectInfo{cullingObjectBuffers[frame].handle(), 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo indirectInfo{update.indirectBuffer.handle(), 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo countInfo{update.countBuffer.handle(), 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo uniformInfo{update.uniformBuffer.handle(), 0,
                                                     sizeof(Culling::CullingUniformData)};
            const VkDescriptorBufferInfo candidateInfo{update.candidateBuffer.handle(), 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo candidateCountInfo{update.candidateCountBuffer.handle(), 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo candidateDispatchInfo{update.candidateDispatchBuffer.handle(), 0,
                                                               VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo pageWorkInfo{update.pageWorkBuffer.handle(), 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo rendererInstanceInfo{instanceBuffers[frame].handle(), 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo rendererInstanceIndexInfo{compactGrassInstanceBuffers[frame].handle(), 0,
                                                                   VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo shadowTransformInfo{update.shadowTransformBuffer.handle(), 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo shadowTransformCountInfo{update.shadowTransformCountBuffer.handle(), 0,
                                                                  VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo shadowMaterialInfo{update.shadowMaterialBuffer.handle(), 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo shadowVisibleInstanceInfo{update.shadowVisibleInstanceBuffer.handle(), 0,
                                                                   VK_WHOLE_SIZE};
            std::array<VkWriteDescriptorSet, cullingDescriptorBindingCount> writes{};
            for (uint32_t i = 0; i < writes.size(); ++i) {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = update.set;
                writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
            }
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[0].pBufferInfo = &objectInfo;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[1].pBufferInfo = &indirectInfo;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[2].pBufferInfo = &countInfo;
            writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[3].pImageInfo = &hiZInfo;
            writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[4].pBufferInfo = &uniformInfo;
            writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[5].pBufferInfo = &candidateInfo;
            writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[6].pBufferInfo = &candidateCountInfo;
            writes[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[7].pBufferInfo = &candidateDispatchInfo;
            writes[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[8].pBufferInfo = &pageWorkInfo;
            writes[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[9].pBufferInfo = &rendererInstanceInfo;
            writes[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[10].pBufferInfo = &rendererInstanceIndexInfo;
            writes[11].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[11].pBufferInfo = &shadowTransformInfo;
            writes[12].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[12].pBufferInfo = &shadowTransformCountInfo;
            writes[13].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[13].pBufferInfo = &shadowMaterialInfo;
            writes[14].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[14].pBufferInfo = &shadowVisibleInstanceInfo;
            // Only the two VSM streams write these, each into its own lists;
            // the camera sets bind the one-sided lists to keep the layout complete.
            const VkDescriptorBufferInfo meshletObjectInfo{update.meshletObjectBuffer.handle(), 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo meshletObjectCountInfo{update.meshletObjectCountBuffer.handle(), 0,
                                                                VK_WHOLE_SIZE};
            writes[15].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[15].pBufferInfo = &meshletObjectInfo;
            writes[16].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[16].pBufferInfo = &meshletObjectCountInfo;
            vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        };
        VkDescriptorSet cameraCullSet = cullSets[frame];
        VkDescriptorSet foliageCullSet = cullSets[MAX_FRAMES_IN_FLIGHT + frame];
        VkDescriptorSet sceneCullSet = cullSets[MAX_FRAMES_IN_FLIGHT * 2 + frame];
        VkDescriptorSet sceneFoliageCullSet = cullSets[MAX_FRAMES_IN_FLIGHT * 3 + frame];
        VkDescriptorSet shadowCullSet = cullSets[MAX_FRAMES_IN_FLIGHT * 4 + frame];
        VkDescriptorSet shadowTwoSidedCullSet = cullSets[MAX_FRAMES_IN_FLIGHT * 5 + frame];
        const auto& candidates = shadowCandidateBuffers[frame];
        const auto& candidateCounts = shadowCandidateCountBuffers[frame];
        const auto& candidateDispatches = shadowCandidateDispatchBuffers[frame];
        const auto& pageWork = shadowPageWorkBuffers[frame];
        const auto& shadowTransforms = shadowInstanceTransformBuffers[frame];
        const auto& shadowTransformCounts = shadowInstanceTransformCountBuffers[frame];
        const auto& shadowTwoSidedTransforms = shadowTwoSidedInstanceTransformBuffers[frame];
        const auto& shadowTwoSidedTransformCounts = shadowTwoSidedInstanceTransformCountBuffers[frame];
        const auto& shadowMaterials = shadowInstanceMaterialBuffers[frame];
        const auto& shadowTwoSidedMaterials = shadowTwoSidedInstanceMaterialBuffers[frame];
        updateCullingSet({cameraCullSet, indirectBuffers[frame], drawCountBuffers[frame], cullingUniformBuffers[frame],
                          candidates, candidateCounts, candidateDispatches, pageWork, shadowTransforms,
                          shadowTransformCounts, shadowMaterials, shadowVisibleInstanceBuffers[frame], shadowMeshletObjectBuffers[frame], shadowMeshletObjectCountBuffers[frame]});
        updateCullingSet({foliageCullSet, foliageIndirectBuffers[frame], foliageDrawCountBuffers[frame],
                          foliageCullingUniformBuffers[frame], candidates, candidateCounts, candidateDispatches,
                          pageWork, shadowTransforms, shadowTransformCounts, shadowMaterials,
                          shadowVisibleInstanceBuffers[frame], shadowMeshletObjectBuffers[frame], shadowMeshletObjectCountBuffers[frame]});
        updateCullingSet({shadowCullSet, shadowIndirectBuffers[frame], shadowDrawCountBuffers[frame],
                          shadowCullingUniformBuffers[frame], candidates, candidateCounts, candidateDispatches,
                          pageWork, shadowTransforms, shadowTransformCounts, shadowMaterials,
                          shadowVisibleInstanceBuffers[frame], shadowMeshletObjectBuffers[frame], shadowMeshletObjectCountBuffers[frame]});
        updateCullingSet({shadowTwoSidedCullSet, shadowTwoSidedIndirectBuffers[frame],
                          shadowTwoSidedDrawCountBuffers[frame], shadowTwoSidedCullingUniformBuffers[frame],
                          shadowTwoSidedCandidateBuffers[frame], shadowTwoSidedCandidateCountBuffers[frame],
                          shadowTwoSidedCandidateDispatchBuffers[frame], pageWork, shadowTwoSidedTransforms,
                          shadowTwoSidedTransformCounts, shadowTwoSidedMaterials,
                          shadowTwoSidedVisibleInstanceBuffers[frame], shadowTwoSidedMeshletObjectBuffers[frame], shadowTwoSidedMeshletObjectCountBuffers[frame]});
        updateCullingSet({sceneCullSet, sceneIndirectBuffers[frame], sceneDrawCountBuffers[frame],
                          sceneCullingUniformBuffers[frame], candidates, candidateCounts, candidateDispatches, pageWork,
                          shadowTransforms, shadowTransformCounts, shadowMaterials,
                          shadowVisibleInstanceBuffers[frame], shadowMeshletObjectBuffers[frame], shadowMeshletObjectCountBuffers[frame]});
        updateCullingSet({sceneFoliageCullSet, sceneFoliageIndirectBuffers[frame], sceneFoliageDrawCountBuffers[frame],
                          sceneFoliageCullingUniformBuffers[frame], candidates, candidateCounts, candidateDispatches,
                          pageWork, shadowTransforms, shadowTransformCounts, shadowMaterials,
                          shadowVisibleInstanceBuffers[frame], shadowMeshletObjectBuffers[frame], shadowMeshletObjectCountBuffers[frame]});
        // Keep this indexed by descriptor binding. Binding 8 is the
        // Hi-Z image, while binding 9 is the cluster hierarchy; the
        // previous compact initializer put the hierarchy at index 8
        // and made vkUpdateDescriptorSets read past the array.
        const std::array<VkDescriptorBufferInfo, 10> meshletInfos{{
            {meshletBuffer.handle(), 0, VK_WHOLE_SIZE},
            {gpuSceneInstanceBuffers[frame].handle(), 0, VK_WHOLE_SIZE},
            {gpuSceneMeshBuffers[frame].handle(), 0, VK_WHOLE_SIZE},
            {visibleInstanceBuffers[frame].handle(), 0, VK_WHOLE_SIZE},
            {visibleInstanceCountBuffers[frame].handle(), 0, sizeof(std::uint32_t)},
            {visibleMeshletBuffers[frame].handle(), 0, VK_WHOLE_SIZE},
            {visibleMeshletCountBuffers[frame].handle(), 0, sizeof(std::uint32_t)},
            {meshletCullingUniformBuffers[frame].handle(), 0, sizeof(Culling::MeshletCullUniforms)},
            {}, // binding 8 is meshletHiZInfo below
            {meshletClusterBuffer.handle(), 0, VK_WHOLE_SIZE},
        }};
        // Meshlet culling keeps binding 8 populated even when the
        // optional Hi-Z pyramid was not allocated for this renderer
        // configuration.  A valid depth fallback is required because
        // nullDescriptor is not enabled on the device.
        const auto& meshletDepthFallback = msaa.enabled() ? hiZDepthBuffer : depthBuffer;
        const VkDescriptorImageInfo meshletHiZInfo =
            hiZBuffer.image() != VK_NULL_HANDLE
                ? VkDescriptorImageInfo{hiZBuffer.sampler(), hiZBuffer.fullView(),
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
                : VkDescriptorImageInfo{meshletDepthFallback.sampler(), meshletDepthFallback.imageView(),
                                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet meshletWrites[10]{};
        for (std::uint32_t binding = 0; binding < std::size(meshletWrites); ++binding) {
            meshletWrites[binding] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                      .dstSet = meshletCullSets[frame],
                                      .dstBinding = binding,
                                      .descriptorCount = 1,
                                      .descriptorType = binding == 8
                                                            ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                            : (binding == 7 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                                            : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
                                      .pImageInfo = binding == 8 ? &meshletHiZInfo : nullptr,
                                      .pBufferInfo = binding == 8 ? nullptr : &meshletInfos[binding]};
        }
        vkUpdateDescriptorSets(device, std::size(meshletWrites), meshletWrites, 0, nullptr);
        const VkDescriptorBufferInfo meshletDispatchInfos[] = {
            {visibleInstanceCountBuffers[frame].handle(), 0, sizeof(std::uint32_t)},
            {meshletCullDispatchBuffers[frame].handle(), 0, sizeof(VkDispatchIndirectCommand)},
        };
        VkWriteDescriptorSet meshletDispatchWrites[2]{};
        for (std::uint32_t binding = 0; binding < std::size(meshletDispatchWrites); ++binding) {
            meshletDispatchWrites[binding] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                              .dstSet = meshletDispatchSets[frame],
                                              .dstBinding = binding,
                                              .descriptorCount = 1,
                                              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                              .pBufferInfo = &meshletDispatchInfos[binding]};
        }
        vkUpdateDescriptorSets(device, std::size(meshletDispatchWrites), meshletDispatchWrites, 0, nullptr);
        instanceCullingPasses[frame].create(instanceCullingPipeline, instanceCullingPipelineLayout,
                                            instanceCullSets[frame], visibleInstanceCountBuffers[frame].handle(),
                                            visibleInstanceBuffers[frame].handle());
        gpuCullingPasses[frame].create(device, cullingPipeline, cullingPipelineLayout, cullSets[frame],
                                       indirectBuffers[frame].handle(), drawCountBuffers[frame].handle(), passCapacity);
        indirectDraws[frame].create(indirectBuffers[frame].handle(), drawCountBuffers[frame].handle(), passCapacity);
        foliageGpuCullingPasses[frame].create(device, cullingPipeline, cullingPipelineLayout, foliageCullSet,
                                              foliageIndirectBuffers[frame].handle(),
                                              foliageDrawCountBuffers[frame].handle(), passCapacity);
        // Grass is culled as terrain chunks in the production draw
        // path.  The experimental GPU-scene per-instance list remains
        // available for the compaction passes, but must not replace a
        // valid chunk command list until its bounds source is shared
        // with terrain extraction.
        foliageIndirectDraws[frame].create(foliageIndirectBuffers[frame].handle(),
                                           foliageDrawCountBuffers[frame].handle(), passCapacity);
        sceneGpuCullingPasses[frame].create(device, cullingPipeline, cullingPipelineLayout, sceneCullSet,
                                            sceneIndirectBuffers[frame].handle(), sceneDrawCountBuffers[frame].handle(),
                                            passCapacity);
        sceneIndirectDraws[frame].create(sceneIndirectBuffers[frame].handle(), sceneDrawCountBuffers[frame].handle(),
                                         passCapacity);
        sceneFoliageGpuCullingPasses[frame].create(device, cullingPipeline, cullingPipelineLayout, sceneFoliageCullSet,
                                                   sceneFoliageIndirectBuffers[frame].handle(),
                                                   sceneFoliageDrawCountBuffers[frame].handle(), passCapacity);
        sceneFoliageIndirectDraws[frame].create(sceneFoliageIndirectBuffers[frame].handle(),
                                                sceneFoliageDrawCountBuffers[frame].handle(), passCapacity);
        shadowCullingPasses[frame].create(device, cullingPipeline, cullingPipelineLayout, shadowCullSet,
                                          shadowIndirectBuffers[frame].handle(), shadowDrawCountBuffers[frame].handle(),
                                          shadowPassCapacity, shadowCandidateCountBuffers[frame].handle(),
                                          shadowCandidateDispatchBuffers[frame].handle(), &shadowPageWorkBuffers[frame],
                                          shadowInstanceTransformCountBuffers[frame].handle());
        shadowIndirectDraws[frame].create(shadowIndirectBuffers[frame].handle(), shadowDrawCountBuffers[frame].handle(),
                                          shadowPassCapacity);
        shadowTwoSidedCullingPasses[frame].create(
            device, cullingPipeline, cullingPipelineLayout, shadowTwoSidedCullSet,
            shadowTwoSidedIndirectBuffers[frame].handle(), shadowTwoSidedDrawCountBuffers[frame].handle(),
            shadowTwoSidedPassCapacity, shadowTwoSidedCandidateCountBuffers[frame].handle(),
            shadowTwoSidedCandidateDispatchBuffers[frame].handle(), &shadowPageWorkBuffers[frame],
            shadowTwoSidedInstanceTransformCountBuffers[frame].handle());
        shadowTwoSidedIndirectDraws[frame].create(shadowTwoSidedIndirectBuffers[frame].handle(),
                                                  shadowTwoSidedDrawCountBuffers[frame].handle(), shadowTwoSidedPassCapacity);
    }
    for (std::uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame)
        refreshGPUSceneDescriptors(frame);
    hiZValid.fill(false);
    [[maybe_unused]] const UploadTicket ticket = uploadBatch.submit();
}

void destroyCullingResources() noexcept {
    for (auto& hiZPass : hiZPasses)
        hiZPass.destroy();
    for (auto& draw : indirectDraws)
        draw.destroy();
    for (auto& draw : foliageIndirectDraws)
        draw.destroy();
    for (auto& draw : sceneIndirectDraws)
        draw.destroy();
    for (auto& draw : sceneFoliageIndirectDraws)
        draw.destroy();
    for (auto& draw : shadowIndirectDraws)
        draw.destroy();
    for (auto& draw : shadowTwoSidedIndirectDraws)
        draw.destroy();
    for (auto& draw : meshletIndexedIndirectDraws)
        draw.destroy();
    for (Buffer& buffer : cullingUniformBuffers)
        buffer.destroy();
    for (Buffer& buffer : sceneCullingUniformBuffers)
        buffer.destroy();
    for (Buffer& buffer : foliageCullingUniformBuffers)
        buffer.destroy();
    for (Buffer& buffer : shadowCullingUniformBuffers)
        buffer.destroy();
    for (Buffer& buffer : shadowTwoSidedCullingUniformBuffers)
        buffer.destroy();
    for (Buffer& buffer : indirectBuffers)
        buffer.destroy();
    for (Buffer& buffer : foliageIndirectBuffers)
        buffer.destroy();
    for (Buffer& buffer : shadowIndirectBuffers)
        buffer.destroy();
    for (Buffer& buffer : shadowTwoSidedIndirectBuffers)
        buffer.destroy();
    for (Buffer& buffer : shadowCandidateBuffers)
        buffer.destroy();
    for (Buffer& buffer : shadowTwoSidedCandidateBuffers)
        buffer.destroy();
    for (Buffer& buffer : shadowCandidateCountBuffers)
        buffer.destroy();
    for (Buffer& buffer : shadowTwoSidedCandidateCountBuffers)
        buffer.destroy();
    for (Buffer& buffer : shadowCandidateDispatchBuffers)
        buffer.destroy();
    for (Buffer& buffer : shadowTwoSidedCandidateDispatchBuffers)
        buffer.destroy();
    for (Buffer& buffer : shadowInstanceTransformBuffers)
        buffer.destroy();
    for (auto* buffers : {&shadowMeshletObjectBuffers, &shadowMeshletObjectCountBuffers, &shadowMeshletCommandBuffers,
                          &shadowMeshletCommandCountBuffers, &shadowMeshletTaskCommandBuffers,
                          &shadowTwoSidedMeshletObjectBuffers, &shadowTwoSidedMeshletObjectCountBuffers,
                          &shadowTwoSidedMeshletCommandBuffers, &shadowTwoSidedMeshletCommandCountBuffers,
                          &shadowTwoSidedMeshletTaskCommandBuffers})
        for (Buffer& buffer : *buffers)
            buffer.destroy();
    shadowMeshletRefineSets.fill(VK_NULL_HANDLE);
    shadowMeshletDrawSets.fill(VK_NULL_HANDLE);
    shadowTwoSidedMeshletRefineSets.fill(VK_NULL_HANDLE);
    shadowTwoSidedMeshletDrawSets.fill(VK_NULL_HANDLE);
    shadowMeshletPassCapacity = 0;
    shadowTwoSidedMeshletPassCapacity = 0;
    for (Buffer& buffer : shadowVisibleInstanceBuffers)
        buffer.destroy();
    for (Buffer& buffer : shadowTwoSidedInstanceTransformBuffers)
        buffer.destroy();
    for (Buffer& buffer : shadowTwoSidedVisibleInstanceBuffers)
        buffer.destroy();
    for (Buffer& buffer : shadowInstanceTransformCountBuffers)
        buffer.destroy();
    for (Buffer& buffer : shadowTwoSidedInstanceTransformCountBuffers)
        buffer.destroy();
    for (Buffer& buffer : shadowInstanceMaterialBuffers)
        buffer.destroy();
    for (Buffer& buffer : shadowTwoSidedInstanceMaterialBuffers)
        buffer.destroy();
    for (Buffer& buffer : shadowPageWorkBuffers)
        buffer.destroy();
    for (Buffer& buffer : vsmRequestedPageBuffers)
        buffer.destroy();
    for (Buffer& buffer : vsmCompactedPageBuffers)
        buffer.destroy();
    for (Buffer& buffer : vsmCompactedPageCountBuffers)
        buffer.destroy();
    for (Buffer& buffer : vsmPageMarkingUniformBuffers)
        buffer.destroy();
    vsmRequestsReady.fill(false);
    vsmRequestsFromScene.fill(false);
    sceneVsmFeedbackPending.fill(false);
    sceneVsmFeedbackCache = {};
    sceneVsmReceiverFeedbackValid = false;
    sceneVsmReceiverRequests.clear();
    for (Buffer& buffer : clusteredLightRangeBuffers)
        buffer.destroy();
    for (Buffer& buffer : clusteredLightIndexBuffers)
        buffer.destroy();
    for (Buffer& buffer : sceneClusteredLightRangeBuffers)
        buffer.destroy();
    for (Buffer& buffer : sceneClusteredLightIndexBuffers)
        buffer.destroy();
    for (Buffer& buffer : clusteredLightingUniformBuffers)
        buffer.destroy();
    for (Buffer& buffer : localLightBuffers)
        buffer.destroy();
    for (Buffer& buffer : sceneClusteredLightingUniformBuffers)
        buffer.destroy();
    for (Buffer& buffer : drawCountBuffers)
        buffer.destroy();
    for (Buffer& buffer : foliageDrawCountBuffers)
        buffer.destroy();
    for (Buffer& buffer : shadowDrawCountBuffers)
        buffer.destroy();
    for (Buffer& buffer : shadowTwoSidedDrawCountBuffers)
        buffer.destroy();
    for (Buffer& buffer : cullingObjectBuffers)
        buffer.destroy();
    for (Buffer& buffer : visibleInstanceBuffers)
        buffer.destroy();
    for (Buffer& buffer : visibleInstanceCountBuffers)
        buffer.destroy();
    for (Buffer& buffer : meshletCullDispatchBuffers)
        buffer.destroy();
    for (Buffer& buffer : visibleMeshletBuffers)
        buffer.destroy();
    for (Buffer& buffer : visibleMeshletCountBuffers)
        buffer.destroy();
    for (Buffer& buffer : meshletIndexedIndirectBuffers)
        buffer.destroy();
    for (Buffer& buffer : meshletIndexedDrawCountBuffers)
        buffer.destroy();
    for (auto* buffers : {&meshletDrawListBuffers, &meshletDrawCommandBuffers})
        for (Buffer& buffer : *buffers)
            buffer.destroy();
    meshletDrawBinSets.fill(VK_NULL_HANDLE);
    meshletDrawCommandSets.fill(VK_NULL_HANDLE);
    meshletDrawSets.fill(VK_NULL_HANDLE);
    for (Buffer& buffer : meshletCullingUniformBuffers)
        buffer.destroy();
    for (Buffer& buffer : grassBinCountBuffers)
        buffer.destroy();
    for (Buffer& buffer : grassBinOffsetBuffers)
        buffer.destroy();
    for (Buffer& buffer : grassBinCursorBuffers)
        buffer.destroy();
    for (Buffer& buffer : generatedGrassInstanceBuffers)
        buffer.destroy();
    for (Buffer& buffer : grassDeformationBuffers)
        buffer.destroy();
    for (Buffer& buffer : grassIndirectBuffers)
        buffer.destroy();
    for (Buffer& buffer : grassDrawCountBuffers)
        buffer.destroy();
    for (Buffer& buffer : grassShadowPageMatricesBuffers)
        buffer.destroy();
    for (Buffer& buffer : grassShadowPageIndirectBuffers)
        buffer.destroy();
    for (Buffer& buffer : grassShadowPageDrawCountBuffers)
        buffer.destroy();
    for (Buffer& buffer : grassShadowPageCullUniformBuffers)
        buffer.destroy();
    for (Buffer& buffer : grassIndirectUniformBuffers)
        buffer.destroy();
    for (Buffer& buffer : grassPrefixUniformBuffers)
        buffer.destroy();
    for (Buffer& buffer : grassClassifyUniformBuffers)
        buffer.destroy();
    for (Buffer& buffer : grassPackedCullUniformBuffers)
        buffer.destroy();
    for (auto& streamUniforms : grassPackedStreamUniformBuffers) {
        for (Buffer& buffer : streamUniforms)
            buffer.destroy();
    }
    for (GrassRenderLists& lists : grassRenderLists) {
        lists.visibleInstances.destroy();
        lists.visibleCount.destroy();
        lists.visibleClusters.destroy();
        lists.visibleClusterCount.destroy();
        lists.bladeCullDispatch.destroy();
        lists.dispatchIndirect.destroy();
        lists.mainVisibleInstances.destroy();
        lists.shadowVisibleInstances.destroy();
        lists.velocityVisibleInstances.destroy();
        lists.classifyCounts.destroy();
        for (Buffer& buffer : lists.binCounts)
            buffer.destroy();
        for (Buffer& buffer : lists.binOffsets)
            buffer.destroy();
        for (Buffer& buffer : lists.binCursors)
            buffer.destroy();
        for (Buffer& buffer : lists.drawInstances)
            buffer.destroy();
        lists.mainIndirect.destroy();
        lists.mainDrawCount.destroy();
        lists.shadowIndirect.destroy();
        lists.shadowDrawCount.destroy();
        lists.velocityIndirect.destroy();
        lists.velocityDrawCount.destroy();
    }
    if (cullingDescriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, cullingDescriptorPool, nullptr);
    }
    if (hiZCopyPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, hiZCopyPipeline, nullptr);
    }
    if (hiZReducePipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, hiZReducePipeline, nullptr);
    }
    if (cullingPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, cullingPipeline, nullptr);
    }
    if (instanceCullingPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, instanceCullingPipeline, nullptr);
    }
    if (meshletCullingPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, meshletCullingPipeline, nullptr);
    }
    if (meshletDispatchPipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(device, meshletDispatchPipeline, nullptr);
    if (meshletIndirectPipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(device, meshletIndirectPipeline, nullptr);
    for (VkPipeline* pipeline : {&meshletDrawBinPipeline, &meshletDrawCommandPipeline,
                                 &shadowMeshletMeshRefinePipeline, &shadowMeshletMeshCommandPipeline}) {
        if (*pipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(device, *pipeline, nullptr);
        *pipeline = VK_NULL_HANDLE;
    }
    if (grassBuildPipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(device, grassBuildPipeline, nullptr);
    if (grassDispatchBuildPipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(device, grassDispatchBuildPipeline, nullptr);
    if (grassPrefixPipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(device, grassPrefixPipeline, nullptr);
    if (grassScatterPipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(device, grassScatterPipeline, nullptr);
    if (grassFinalizePipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(device, grassFinalizePipeline, nullptr);
    if (grassPackedCullPipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(device, grassPackedCullPipeline, nullptr);
    if (grassShadowPageCullPipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(device, grassShadowPageCullPipeline, nullptr);
    if (grassBladeCullPipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(device, grassBladeCullPipeline, nullptr);
    if (grassClassifyPipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(device, grassClassifyPipeline, nullptr);
    if (grassPackedBinPipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(device, grassPackedBinPipeline, nullptr);
    if (grassPackedPrefixPipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(device, grassPackedPrefixPipeline, nullptr);
    if (grassPackedScatterPipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(device, grassPackedScatterPipeline, nullptr);
    if (grassPackedFinalizePipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(device, grassPackedFinalizePipeline, nullptr);
    if (clusteredLightingPipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(device, clusteredLightingPipeline, nullptr);
    if (vsmPageMarkingPipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(device, vsmPageMarkingPipeline, nullptr);
    if (vsmPageCompactPipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(device, vsmPageCompactPipeline, nullptr);
    if (shadowMeshletRefinePipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(device, shadowMeshletRefinePipeline, nullptr);
    shadowMeshletRefinePipeline = VK_NULL_HANDLE;
    if (hiZCopyPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, hiZCopyPipelineLayout, nullptr);
    }
    if (hiZReducePipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, hiZReducePipelineLayout, nullptr);
    }
    if (cullingPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, cullingPipelineLayout, nullptr);
    }
    if (instanceCullingPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, instanceCullingPipelineLayout, nullptr);
    }
    if (meshletCullingPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, meshletCullingPipelineLayout, nullptr);
    }
    if (meshletDispatchPipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, meshletDispatchPipelineLayout, nullptr);
    if (meshletIndirectPipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, meshletIndirectPipelineLayout, nullptr);
    if (grassBuildPipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, grassBuildPipelineLayout, nullptr);
    if (grassDispatchBuildPipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, grassDispatchBuildPipelineLayout, nullptr);
    if (grassPrefixPipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, grassPrefixPipelineLayout, nullptr);
    if (grassScatterPipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, grassScatterPipelineLayout, nullptr);
    if (grassFinalizePipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, grassFinalizePipelineLayout, nullptr);
    if (grassPackedCullPipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, grassPackedCullPipelineLayout, nullptr);
    if (grassShadowPageCullPipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, grassShadowPageCullPipelineLayout, nullptr);
    if (grassBladeCullPipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, grassBladeCullPipelineLayout, nullptr);
    if (grassClassifyPipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, grassClassifyPipelineLayout, nullptr);
    if (grassPackedBinPipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, grassPackedBinPipelineLayout, nullptr);
    if (grassPackedScatterPipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, grassPackedScatterPipelineLayout, nullptr);
    if (grassPackedFinalizePipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, grassPackedFinalizePipelineLayout, nullptr);
    if (clusteredLightingPipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, clusteredLightingPipelineLayout, nullptr);
    if (vsmPageMarkingPipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, vsmPageMarkingPipelineLayout, nullptr);
    if (vsmPageCompactPipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, vsmPageCompactPipelineLayout, nullptr);
    if (shadowMeshletRefinePipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, shadowMeshletRefinePipelineLayout, nullptr);
    shadowMeshletRefinePipelineLayout = VK_NULL_HANDLE;
    if (hiZCopyDescriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, hiZCopyDescriptorSetLayout, nullptr);
    }
    if (hiZReduceDescriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, hiZReduceDescriptorSetLayout, nullptr);
    }
    if (cullingDescriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, cullingDescriptorSetLayout, nullptr);
    }
    if (instanceCullingDescriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, instanceCullingDescriptorSetLayout, nullptr);
    }
    if (meshletCullingDescriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, meshletCullingDescriptorSetLayout, nullptr);
    }
    if (meshletDispatchDescriptorSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device, meshletDispatchDescriptorSetLayout, nullptr);
    if (meshletIndirectDescriptorSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device, meshletIndirectDescriptorSetLayout, nullptr);
    if (grassBuildDescriptorSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device, grassBuildDescriptorSetLayout, nullptr);
    if (grassDispatchBuildDescriptorSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device, grassDispatchBuildDescriptorSetLayout, nullptr);
    if (grassPrefixDescriptorSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device, grassPrefixDescriptorSetLayout, nullptr);
    if (grassScatterDescriptorSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device, grassScatterDescriptorSetLayout, nullptr);
    if (grassFinalizeDescriptorSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device, grassFinalizeDescriptorSetLayout, nullptr);
    if (grassPackedCullDescriptorSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device, grassPackedCullDescriptorSetLayout, nullptr);
    if (grassShadowPageCullDescriptorSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device, grassShadowPageCullDescriptorSetLayout, nullptr);
    if (grassBladeCullDescriptorSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device, grassBladeCullDescriptorSetLayout, nullptr);
    if (grassClassifyDescriptorSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device, grassClassifyDescriptorSetLayout, nullptr);
    if (grassPackedBinDescriptorSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device, grassPackedBinDescriptorSetLayout, nullptr);
    if (grassPackedScatterDescriptorSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device, grassPackedScatterDescriptorSetLayout, nullptr);
    if (grassPackedFinalizeDescriptorSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device, grassPackedFinalizeDescriptorSetLayout, nullptr);
    if (clusteredLightingDescriptorSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device, clusteredLightingDescriptorSetLayout, nullptr);
    if (vsmPageMarkingDescriptorSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device, vsmPageMarkingDescriptorSetLayout, nullptr);
    if (vsmPageCompactDescriptorSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device, vsmPageCompactDescriptorSetLayout, nullptr);
    if (shadowMeshletRefineDescriptorSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device, shadowMeshletRefineDescriptorSetLayout, nullptr);
    shadowMeshletRefineDescriptorSetLayout = VK_NULL_HANDLE;
    cullingDescriptorPool = VK_NULL_HANDLE;
    hiZCopyPipeline = hiZReducePipeline = cullingPipeline = VK_NULL_HANDLE;
    vsmPageMarkingPipeline = VK_NULL_HANDLE;
    vsmPageCompactPipeline = VK_NULL_HANDLE;
    instanceCullingPipeline = VK_NULL_HANDLE;
    meshletCullingPipeline = VK_NULL_HANDLE;
    meshletDispatchPipeline = VK_NULL_HANDLE;
    meshletIndirectPipeline = VK_NULL_HANDLE;
    grassBuildPipeline = grassPrefixPipeline = grassScatterPipeline = grassFinalizePipeline = VK_NULL_HANDLE;
    grassPackedCullPipeline = grassBladeCullPipeline = grassClassifyPipeline = VK_NULL_HANDLE;
    grassShadowPageCullPipeline = VK_NULL_HANDLE;
    grassPackedBinPipeline = grassPackedPrefixPipeline = grassPackedScatterPipeline = grassPackedFinalizePipeline =
        VK_NULL_HANDLE;
    hiZCopyPipelineLayout = hiZReducePipelineLayout = cullingPipelineLayout = VK_NULL_HANDLE;
    vsmPageMarkingPipelineLayout = VK_NULL_HANDLE;
    vsmPageCompactPipelineLayout = VK_NULL_HANDLE;
    instanceCullingPipelineLayout = VK_NULL_HANDLE;
    meshletCullingPipelineLayout = VK_NULL_HANDLE;
    meshletDispatchPipelineLayout = VK_NULL_HANDLE;
    meshletIndirectPipelineLayout = VK_NULL_HANDLE;
    grassBuildPipelineLayout = grassPrefixPipelineLayout = grassScatterPipelineLayout = grassFinalizePipelineLayout =
        VK_NULL_HANDLE;
    grassPackedCullPipelineLayout = grassBladeCullPipelineLayout = grassClassifyPipelineLayout = VK_NULL_HANDLE;
    grassShadowPageCullPipelineLayout = VK_NULL_HANDLE;
    grassPackedBinPipelineLayout = grassPackedScatterPipelineLayout = grassPackedFinalizePipelineLayout =
        VK_NULL_HANDLE;
    hiZCopyDescriptorSetLayout = hiZReduceDescriptorSetLayout = cullingDescriptorSetLayout = VK_NULL_HANDLE;
    vsmPageMarkingDescriptorSetLayout = VK_NULL_HANDLE;
    vsmPageCompactDescriptorSetLayout = VK_NULL_HANDLE;
    instanceCullingDescriptorSetLayout = VK_NULL_HANDLE;
    meshletCullingDescriptorSetLayout = VK_NULL_HANDLE;
    meshletDispatchDescriptorSetLayout = VK_NULL_HANDLE;
    meshletIndirectDescriptorSetLayout = VK_NULL_HANDLE;
    grassBuildDescriptorSetLayout = grassPrefixDescriptorSetLayout = grassScatterDescriptorSetLayout =
        grassFinalizeDescriptorSetLayout = VK_NULL_HANDLE;
    grassPackedCullDescriptorSetLayout = grassBladeCullDescriptorSetLayout = grassClassifyDescriptorSetLayout =
        VK_NULL_HANDLE;
    grassShadowPageCullDescriptorSetLayout = VK_NULL_HANDLE;
    grassPackedBinDescriptorSetLayout = grassPackedScatterDescriptorSetLayout = grassPackedFinalizeDescriptorSetLayout =
        VK_NULL_HANDLE;
    for (auto& hiZBuffer : hiZBuffers)
        hiZBuffer.destroy();
    gpuObjects.clear();
    hiZValid.fill(false);
}

void createFramebuffers() {
    const VkExtent2D extent = swapchain.extent();
    // The prepass writes velocity and view-normal MRTs at single sample.
    // Velocity is retained even without TAA to keep attachment locations
    // stable: view normals are fragment output location 2.
    if (!msaa.enabled()) {
        const std::array normalFamilies{vulkanDevice.graphicsQueueFamily(), vulkanDevice.computeQueueFamily()};
        const std::span<const std::uint32_t> sharingFamilies =
            vulkanDevice.hasAsyncComputeQueue() && normalFamilies[0] != normalFamilies[1]
                ? std::span<const std::uint32_t>(normalFamilies)
                : std::span<const std::uint32_t>{};
        velocityBuffer.create(vulkanDevice.physical(), device, extent, vulkanDevice.allocator(), VK_FILTER_NEAREST,
                              VK_FORMAT_R16G16_SFLOAT);
        gtaoViewNormalBuffer.create(vulkanDevice.physical(), device, extent, vulkanDevice.allocator(),
                                    VK_FILTER_NEAREST, VK_FORMAT_R16G16_SNORM, false, sharingFamilies);
    }
    createVirtualWaterResources();
}

void createVirtualWaterResources() {
    std::array<VkBuffer, MAX_FRAMES_IN_FLIGHT> instances{};
    std::array<VkBuffer, MAX_FRAMES_IN_FLIGHT> culling{};
    for (std::size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        instances[i] = instanceBuffers[i].handle();
        culling[i] = cullingUniformBuffers[i].handle();
    }
    const DepthBuffer& sampledDepth = msaa.enabled() ? hiZDepthBuffer : depthBuffer;
    const VkDescriptorImageInfo depthInfo{sampledDepth.sampler(), sampledDepth.imageView(),
                                          VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    std::array<VkDescriptorImageInfo, MAX_FRAMES_IN_FLIGHT> hiZInfos{};
    for (std::size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        const auto previousHiZFrame = (i + MAX_FRAMES_IN_FLIGHT - 1U) % MAX_FRAMES_IN_FLIGHT;
        hiZInfos[i] = {hiZBuffers[previousHiZFrame].sampler(), hiZBuffers[previousHiZFrame].fullView(),
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        if (hiZInfos[i].imageView == VK_NULL_HANDLE)
            hiZInfos[i] = depthInfo;
    }
    // Without water bodies the renderer never records; its screen-space
    // targets (~76 MiB at 1080p) stay 1x1 until water appears.
    const Water::WaterRenderWorld waterWorld = Water::WaterRenderWorld::capture(registry, sceneGpu);
    virtualWaterRenderer.create(
        vulkanDevice.physical(), device, vulkanDevice.allocator(), assetManager,
        waterWorld.empty() ? VkExtent2D{1U, 1U} : swapchain.extent(),
        sampledDepth.format(), sampledDepth.imageView(), shadowPass.descriptorSetLayout(), hdrBuffer.imageView(),
        {opaqueSceneColor.sampler(), opaqueSceneColor.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, depthInfo,
        hiZInfos, instances, culling);
    virtualWaterRenderer.rebuild(waterWorld);
}

void destroyVelocityResources() noexcept {
    velocityBuffer.destroy();
    gtaoViewNormalBuffer.destroy();
}

void createSceneViewportResources() {
    // Start with the drawable extent. The editor will later supply its
    // panel extent through the renderer viewport API; creating it here
    // also makes the off-screen lifecycle valid for non-editor users.
    sceneViewportTarget.create(vulkanDevice.physical(), device, swapchain.extent(), vulkanDevice.allocator());
    sceneViewportDepthBuffer.create(sceneViewportTarget.extent(), VK_SAMPLE_COUNT_1_BIT,
                                    msaa.enabled() ? hiZDepthBuffer.format() : depthBuffer.format());
    sceneViewportDepthInitialized = false;
    createSceneViewportPrepassTargets();
    createSceneViewportForwardPass();
    createSceneVirtualWaterResources();
}

void createSceneViewportPrepassTargets() {
    sceneViewportVelocity.destroy();
    sceneViewportNormals.destroy();
    sceneViewportVelocity.create(vulkanDevice.physical(), device, sceneViewportTarget.extent(),
                                 vulkanDevice.allocator(), VK_FILTER_NEAREST, VK_FORMAT_R16G16_SFLOAT);
    sceneViewportNormals.create(vulkanDevice.physical(), device, sceneViewportTarget.extent(), vulkanDevice.allocator(),
                                VK_FILTER_NEAREST, VK_FORMAT_R16G16_SNORM);
}

void createSceneVirtualWaterResources() {
    sceneVirtualWaterRenderer.destroy();
    sceneGlassPass.destroy();
    sceneGlassPassInfo = GlassPass::CreateInfo{.physicalDevice = vulkanDevice.physical(),
                           .device = device,
                           .allocator = vulkanDevice.allocator(),
                           .assets = &assetManager,
                           .sceneLayout = sceneDescriptorPass.descriptorSetLayout(),
                           .extent = sceneViewportTarget.extent(),
                           .depthFormat = sceneViewportDepthBuffer.format(),
                           .sceneDepthView = sceneViewportDepthBuffer.imageView(),
                           .sceneDepthSampler = sceneViewportDepthBuffer.sampler()};
    sceneOpaqueColor.destroy();
    sceneOpaqueColor.create(vulkanDevice.physical(), device, sceneViewportTarget.extent(), vulkanDevice.allocator());
    sceneOpaqueColorInitialized = false;

    std::array<VkBuffer, MAX_FRAMES_IN_FLIGHT> instances{};
    std::array<VkBuffer, MAX_FRAMES_IN_FLIGHT> culling{};
    for (std::size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        instances[i] = instanceBuffers[i].handle();
        culling[i] = sceneCullingUniformBuffers[i].handle();
    }
    const DepthBuffer& sampledDepth = sceneViewportDepthBuffer;
    const VkDescriptorImageInfo depthInfo{sampledDepth.sampler(), sampledDepth.imageView(),
                                          VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    std::array<VkDescriptorImageInfo, MAX_FRAMES_IN_FLIGHT> noHiZInfos{};
    noHiZInfos.fill(depthInfo);
    // Bind depth to keep the descriptor valid, but deliberately turn
    // Hi-Z off: the Scene View owns no independent pyramid yet.
    const Water::WaterRenderWorld sceneWaterWorld = Water::WaterRenderWorld::capture(registry, sceneGpu);
    sceneVirtualWaterRenderer.create(
        vulkanDevice.physical(), device, vulkanDevice.allocator(), assetManager,
        sceneWaterWorld.empty() ? VkExtent2D{1U, 1U} : sceneViewportTarget.extent(),
        sampledDepth.format(), sampledDepth.imageView(), sceneDescriptorPass.descriptorSetLayout(),
        sceneViewportTarget.color().imageView(),
        {sceneOpaqueColor.sampler(), sceneOpaqueColor.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, depthInfo,
        noHiZInfos, instances, culling, false);
    sceneVirtualWaterRenderer.rebuild(sceneWaterWorld);
}

void applyPendingSceneViewportResize() {
    const VkExtent2D requested = requestedSceneViewportExtent;
    if (requested.width == 0 || requested.height == 0 ||
        (requested.width == sceneViewportTarget.extent().width &&
         requested.height == sceneViewportTarget.extent().height))
        return;

    // Resolution changes replace color, depth and water attachments at
    // once. This path is user-driven and infrequent, so wait for every
    // queue explicitly; frame fences do not cover every submission
    // made by optional async renderer paths on all drivers.
    waitIdle();
    sceneViewportTarget.resize(requested);
    sceneViewportDepthBuffer.create(requested, VK_SAMPLE_COUNT_1_BIT,
                                    msaa.enabled() ? hiZDepthBuffer.format() : depthBuffer.format());
    sceneViewportDepthInitialized = false;
    createSceneViewportPrepassTargets();
    createSceneDirectionalVisibilityPass();
    createSceneVirtualWaterResources();

    if (sceneViewportDescriptor != VK_NULL_HANDLE) {
        // Recent ImGui Vulkan backends use a SAMPLED_IMAGE descriptor
        // plus a separate sampler. Its descriptor type is backend
        // owned, so a direct COMBINED_IMAGE_SAMPLER write is invalid.
        if (editorUiBackend)
            editorUiBackend->removeTexture(reinterpret_cast<std::uintptr_t>(sceneViewportDescriptor));
        sceneViewportDescriptor = VK_NULL_HANDLE;
    }
    sceneViewportCacheValid = false;
    sceneViewportImageInitialized = false;
    sceneViewportNeedsRender = true;
}

void destroySceneViewportResources() noexcept {
    sceneVirtualWaterRenderer.destroy();
    sceneGlassPass.destroy();
    sceneOpaqueColor.destroy();
    sceneOpaqueColorInitialized = false;
    sceneViewportDepthBuffer.destroy();
    sceneViewportDepthInitialized = false;
    destroySceneViewportForwardPass();
    sceneViewportVelocity.destroy();
    sceneViewportNormals.destroy();
    sceneViewportTarget.destroy();
}

// Transitional no-op hooks: callers are shared by resize and shader
// reload paths. Dynamic rendering binds the target views directly.
void createSceneViewportFramebuffer() noexcept {
    sceneViewportFramebuffer = VK_NULL_HANDLE;
}

void destroySceneViewportFramebuffer() noexcept {
    sceneViewportFramebuffer = VK_NULL_HANDLE;
}

void createCommandPool() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = vulkanDevice.graphicsQueueFamily();

    if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
        throw std::runtime_error("Could not create command pool");
    }

    if (vulkanDevice.hasAsyncComputeQueue()) {
        poolInfo.queueFamilyIndex = vulkanDevice.computeQueueFamily();
        if (vkCreateCommandPool(device, &poolInfo, nullptr, &asyncComputeCommandPool) != VK_SUCCESS) {
            throw std::runtime_error("Could not create async compute command pool");
        }
    }
    poolInfo.queueFamilyIndex = vulkanDevice.transferQueueFamily();
    if (vkCreateCommandPool(device, &poolInfo, nullptr, &transferCommandPool) != VK_SUCCESS) {
        throw std::runtime_error("Could not create transfer command pool");
    }
}

void createCommandBuffers() {
    commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    postViewportGraphicsCommandBuffers.resize(MAX_FRAMES_IN_FLIGHT);

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers.size());

    if (vkAllocateCommandBuffers(device, &allocInfo, commandBuffers.data()) != VK_SUCCESS) {
        throw std::runtime_error("Could not allocate command buffers");
    }
    if (vkAllocateCommandBuffers(device, &allocInfo, postViewportGraphicsCommandBuffers.data()) != VK_SUCCESS) {
        throw std::runtime_error("Could not allocate post-viewport graphics command buffers");
    }
}

[[nodiscard]] Mat4 lightSpaceMatrix() const {
    const auto light = directionalLight();
    const float extent = sceneRadius;
    const Vec3 lightTarget = sceneCenter;
    const Vec3 lightPosition = lightTarget - (light.direction * (extent * 5.0F));
    constexpr Vec3 worldUp{0.0F, 1.0F, 0.0F};
    const Mat4 lightView = Mat4::lookAt(lightPosition, lightTarget, worldUp);
    const Mat4 lightProjection =
        Mat4::scale(Mat4::ortho(-extent * 2.25F, extent * 2.25F, -extent * 2.25F, extent * 2.25F, 0.1F, extent * 8.0F),
                    Vec3{1.0F, -1.0F, 1.0F});
    return lightProjection * lightView;
}

std::uint32_t updateVirtualShadowClipmaps(const Vec3& cameraPosition,
                                          std::array<Mat4, ShadowMap::ClipLevelCount>& matrices,
                                          Vec3& previousCameraPosition, Vec3& previousLightDirection,
                                          bool& valid) const {
    const DirectionalLight light = directionalLight();
    std::uint32_t updateMask = 0;

    constexpr std::array<float, 6> baseExtents{6.0F, 12.0F, 24.0F, 48.0F, 96.0F, 192.0F};
    // sceneRadius is the largest AABB half-extent, not a sphere radius.
    // Enclose its diagonal for every light orientation. The final level
    // is scene-centred and fits in the permanently requested 10x10 tiles.
    const float sceneBoundingRadius = sceneRadius * std::sqrt(3.0F);
    const float farExtent = std::max(324.0F, sceneBoundingRadius * ShadowMap::FallbackExtentScale);
    const Vec3 direction = light.direction.normalized();
    Vec3 up{0.0F, 1.0F, 0.0F};
    if (std::abs(dot(direction, up)) > 0.98F)
        up = Vec3{0.0F, 0.0F, 1.0F};
    const Vec3 right = cross(direction, up).normalized();
    const Vec3 lightUp = cross(right, direction).normalized();

    for (std::uint32_t level = 0; level < ShadowMap::ClipLevelCount; ++level) {
        const float extent = level < baseExtents.size() ? baseExtents[level] : farExtent;
        const Vec3 clipCenter = level + 1 == ShadowMap::ClipLevelCount ? sceneCenter : cameraPosition;
        // Page-sized snapping makes the virtual address space stable
        // while the camera moves inside a page. Cached physical pages
        // therefore survive ordinary sub-page camera motion.
        const float worldUnitsPerPage = extent * 2.0F / static_cast<float>(ShadowMap::VirtualPagesPerAxis);
        const float snappedX = std::round(dot(clipCenter, right) / worldUnitsPerPage) * worldUnitsPerPage;
        const float snappedY = std::round(dot(clipCenter, lightUp) / worldUnitsPerPage) * worldUnitsPerPage;
        // Keep depth encoding independent of the camera so pages can
        // be remapped when the clipmap scrolls without being redrawn.
        const float alongLight = dot(sceneCenter, direction);
        const float lightDistance = sceneRadius * 2.0F + extent;
        // Construct the basis directly. lookAt(target - direction * d,
        // target) loses precision when subtracting large world positions,
        // changing the depth/basis during what should be a pure scroll.
        glm::mat4 view{1.0F};
        view[0] = {right.x(), lightUp.x(), -direction.x(), 0.0F};
        view[1] = {right.y(), lightUp.y(), -direction.y(), 0.0F};
        view[2] = {right.z(), lightUp.z(), -direction.z(), 0.0F};
        view[3] = {-snappedX, -snappedY, alongLight - lightDistance, 1.0F};
        const Mat4 lightView{view};
        const Mat4 lightProjection =
            Mat4::scale(Mat4::ortho(-extent, extent, -extent, extent, 0.1F, sceneRadius * 4.0F + extent * 2.0F),
                        Vec3{1.0F, -1.0F, 1.0F});
        const Mat4 next = lightProjection * lightView;
        // Matrix work is tiny compared with rerasterization. Update all
        // changed levels immediately, including slow light motion and
        // scene-bound changes, instead of leaving coarse levels stale.
        if (!valid || matrices[level].native() != next.native()) {
            matrices[level] = next;
            updateMask |= 1u << level;
        }
    }
    previousCameraPosition = cameraPosition;
    previousLightDirection = light.direction;
    valid = true;
    return updateMask;
}
