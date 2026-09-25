#include "Engine/Renderer/Lighting/DDGI/DDGISystem.h"

#include "Engine/Renderer/shader_loader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <vector>

namespace Engine {
namespace {
    // Must match ddgi_common.slang: metadata, packed RGBA8, packed half2.
    constexpr std::uint32_t cacheProbeWords = 328;
    constexpr VkDeviceSize cacheRegionBytes = 64 * cacheProbeWords * sizeof(std::uint32_t);

    struct ProbeUpdate final {
        std::uint32_t probeIndex;
        std::uint32_t framesSinceLastUpdate;
        std::uint32_t relocated;
        std::uint32_t padding;
        std::array<std::uint32_t, 4> localLightIndices;
    };
    static_assert(sizeof(ProbeUpdate) == 32);

    std::int32_t positiveModulo(std::int32_t value, std::int32_t count) {
        return (value % count + count) % count;
    }

    VkImageMemoryBarrier2 imageBarrier(VkImage image, VkImageLayout oldLayout,
                                       VkImageLayout newLayout, VkPipelineStageFlags2 sourceStage,
                                       VkAccessFlags2 sourceAccess, VkPipelineStageFlags2 destinationStage,
                                       VkAccessFlags2 destinationAccess) {
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = sourceStage;
        barrier.srcAccessMask = sourceAccess;
        barrier.dstStageMask = destinationStage;
        barrier.dstAccessMask = destinationAccess;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        return barrier;
    }

    void emitImageBarriers(VkCommandBuffer commandBuffer,
                           std::span<const VkImageMemoryBarrier2> barriers) {
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size());
        dependency.pImageMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(commandBuffer, &dependency);
    }

    void computeResourceBarrier(VkCommandBuffer commandBuffer,
                                std::initializer_list<VkBuffer> buffers,
                                std::initializer_list<VkImage> images = {}) {
        std::vector<VkBufferMemoryBarrier2> bufferBarriers;
        bufferBarriers.reserve(buffers.size());
        for (const VkBuffer buffer : buffers) {
            VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = buffer;
            barrier.size = VK_WHOLE_SIZE;
            bufferBarriers.push_back(barrier);
        }
        std::vector<VkImageMemoryBarrier2> imageBarriers;
        imageBarriers.reserve(images.size());
        for (const VkImage image : images)
            imageBarriers.push_back(imageBarrier(image, VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT));
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.bufferMemoryBarrierCount = static_cast<std::uint32_t>(bufferBarriers.size());
        dependency.pBufferMemoryBarriers = bufferBarriers.data();
        dependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(imageBarriers.size());
        dependency.pImageMemoryBarriers = imageBarriers.data();
        vkCmdPipelineBarrier2(commandBuffer, &dependency);
    }
}

DDGISystem::~DDGISystem() { destroy(); }

void DDGISystem::create(VkPhysicalDevice physical, VkDevice device, VmaAllocator allocator,
                        VkDescriptorSetLayout sceneLayout, Assets::AssetManager& assets) {
    if (created()) return;
    if (!physical || !device || !allocator || !sceneLayout)
        throw std::invalid_argument("DDGI requires a Vulkan device and scene layout");
    device_ = device;
    try {
        constexpr std::array<float, 3> spacings{2.0F, 6.0F, 18.0F};
        constexpr std::array<std::uint32_t, 3> rays{256, 160, 96};
        for (std::size_t cascade = 0; cascade < volumes_.size(); ++cascade) {
            volumes_[cascade].probeSpacing = spacings[cascade];
            volumes_[cascade].raysPerProbe = rays[cascade];
            // Keep traced rays long enough for lighting; visibility moments
            // are clamped separately to the local probe spacing in the shader.
            volumes_[cascade].maxRayDistance = std::min(spacings[cascade] * 16.0F, 252.0F);
        }
        for (auto& cascade : resources_.cascades) {
            auto& frame = cascade.history;
            for (auto& directions : cascade.rayDirections)
                directions.createDeviceLocalEmpty(device, 256 * sizeof(float) * 4,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, allocator);
            cascade.regionCache.createDeviceLocalEmpty(device, cacheRegionCount * cacheRegionBytes,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, allocator);
            cascade.cacheMapping.createDeviceLocalEmpty(device, 4096 * sizeof(std::uint32_t),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, allocator);
            frame.rayData.create(physical, device, {256, 256}, allocator,
                                 VK_FILTER_NEAREST, VK_FORMAT_R16G16B16A16_SFLOAT, true);
            frame.irradiance.create(physical, device, {128, 1024}, allocator,
                                    VK_FILTER_LINEAR, VK_FORMAT_R8G8B8A8_UNORM, true);
            frame.distance.create(physical, device, {256, 2048}, allocator,
                                  VK_FILTER_LINEAR, VK_FORMAT_R16G16_SFLOAT, true);
            frame.fixedRayData.create(physical, device, {32, 256}, allocator,
                                      VK_FILTER_NEAREST, VK_FORMAT_R16G16B16A16_SFLOAT, true);
            frame.probeData.create(physical, device, {16, 128}, allocator,
                                   VK_FILTER_NEAREST, VK_FORMAT_R16G16B16A16_SFLOAT, true);
            frame.probeStates.createDeviceLocalEmpty(device, 2048 * 20,
                                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, allocator);
            frame.updateList.createDeviceLocalEmpty(device, 256 * sizeof(ProbeUpdate),
                                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, allocator);
        }
        const std::array<VkDescriptorSetLayoutBinding, 19> bindings{{
            {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {10, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {12, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {13, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {14, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {15, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {17, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {18, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        }};
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &layout_) != VK_SUCCESS)
            throw std::runtime_error("Could not create DDGI descriptor layout");
        const std::array<VkDescriptorPoolSize, 4> poolSizes{{
            {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 6},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 60},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 30},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 18},
        }};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 6;
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &pool_) != VK_SUCCESS)
            throw std::runtime_error("Could not create DDGI descriptor pool");
        const std::array layouts{layout_, layout_, layout_, layout_, layout_, layout_};
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = pool_;
        allocation.descriptorSetCount = 6;
        allocation.pSetLayouts = layouts.data();
        std::array<VkDescriptorSet, 6> allocatedSets{};
        if (vkAllocateDescriptorSets(device_, &allocation, allocatedSets.data()) != VK_SUCCESS)
            throw std::runtime_error("Could not allocate DDGI descriptor sets");
        for (std::size_t cascade = 0; cascade < 3; ++cascade)
            for (std::size_t slot = 0; slot < 2; ++slot)
                sets_[cascade][slot] = allocatedSets[cascade * 2 + slot];
        const std::array pipelineLayouts{sceneLayout, layout_};
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants)};
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 2;
        pipelineLayoutInfo.pSetLayouts = pipelineLayouts.data();
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &push;
        if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS)
            throw std::runtime_error("Could not create DDGI pipeline layout");
        const auto createPipeline = [&](const char* name, VkPipeline& output) {
            const auto shader = Vkutil::loadShaderModule(device_, assets, name);
            VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                          VK_SHADER_STAGE_COMPUTE_BIT, shader.get(), "main", nullptr};
            info.layout = pipelineLayout_;
            if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &output) != VK_SUCCESS)
                throw std::runtime_error("Could not create DDGI compute pipeline");
        };
        createPipeline("shaders/ddgi_trace.spv", tracePipeline_);
        createPipeline("shaders/ddgi_directions.spv", directionsPipeline_);
        createPipeline("shaders/ddgi_schedule.spv", schedulePipeline_);
        createPipeline("shaders/ddgi_finish.spv", finishPipeline_);
        createPipeline("shaders/ddgi_validate.spv", validatePipeline_);
        createPipeline("shaders/ddgi_relocate.spv", relocatePipeline_);
        createPipeline("shaders/ddgi_classify.spv", classifyPipeline_);
        createPipeline("shaders/ddgi_scroll_reset.spv", scrollResetPipeline_);
        createPipeline("shaders/ddgi_cache_store.spv", cacheStorePipeline_);
        createPipeline("shaders/ddgi_blend_irradiance.spv", irradiancePipeline_);
        createPipeline("shaders/ddgi_blend_distance.spv", distancePipeline_);
    } catch (...) { destroy(); throw; }
}

void DDGISystem::record(VkCommandBuffer commandBuffer, std::uint32_t frameSlot,
                        std::uint32_t frameIndex, VkDescriptorSet sceneSet,
                        VkAccelerationStructureKHR tlas, VkBuffer instances,
                        VkBuffer meshes, VkBuffer materials, VkBuffer vertices,
                        VkBuffer indices, const std::array<float, 3>& cameraPosition,
                        const std::array<std::uint64_t, 4>& sceneRevisions,
                        bool sceneGeometryChanged) {
    if (!created() || !commandBuffer || !sceneSet || !tlas || !instances || !meshes ||
        !materials || !vertices || !indices) return;
    frameSlot %= 2;
    // The transform list survives a no-op updateDirty(). Consume it only
    // when its registry revision advances; camera-only edits do not reset GI.
    const bool sceneChanged = !sceneRevisionsInitialized_ ||
        sceneRevisions_[0] != sceneRevisions[0] ||
        sceneRevisions_[1] != sceneRevisions[1] ||
        sceneRevisions_[2] != sceneRevisions[2] ||
        (sceneGeometryChanged && sceneRevisions_[3] != sceneRevisions[3]);
    sceneRevisions_ = sceneRevisions;
    sceneRevisionsInitialized_ = true;
    // Maximum: 672 probes and 119,808 primary probe rays across the cascades.
    // Every probe's ray quota includes 32 fixed validation rays; a relocated
    // probe spends another 32 on revalidation and traces fewer random rays.
    // Each cascade has its own 2048-probe history.
    constexpr std::array<std::uint32_t, 3> cascadeUpdates{256, 224, 192};
    // TLAS was built or updated earlier on this graphics command buffer.
    // Ray queries must wait for the AS writes even though the RenderGraph
    // does not yet model acceleration structures as resources.
    VkMemoryBarrier2 asBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    asBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    asBarrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    asBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    asBarrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    VkDependencyInfo asDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    asDependency.memoryBarrierCount = 1;
    asDependency.pMemoryBarriers = &asBarrier;
    vkCmdPipelineBarrier2(commandBuffer, &asDependency);
    std::array<PushConstants, 3> cascadePushes{};
    for (std::size_t cascadeIndex = 0; cascadeIndex < volumes_.size(); ++cascadeIndex) {
    auto& volume = volumes_[cascadeIndex];
    const std::uint32_t updateCount = cascadeUpdates[cascadeIndex];
    auto& state = cascadeStates_[cascadeIndex];
    const auto previousOrigin = state.originCell;
    const auto previousScroll = state.scrollOffset;
    std::array<std::int32_t, 3> newOriginCell{};
    std::array<std::int32_t, 3> scrollDelta{};
    constexpr std::array<std::int32_t, 3> counts{16, 8, 16};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        newOriginCell[axis] = static_cast<std::int32_t>(
            std::floor(cameraPosition[axis] / volume.probeSpacing)) - counts[axis] / 2;
        scrollDelta[axis] = state.initialized ? newOriginCell[axis] - state.originCell[axis] : 0;
        state.scrollOffset[axis] = positiveModulo(state.scrollOffset[axis] + scrollDelta[axis], counts[axis]);
        volume.origin[axis] = static_cast<float>(newOriginCell[axis]) * volume.probeSpacing;
    }
    state.originCell = newOriginCell;
    volume.scrollOffset = state.scrollOffset;
    auto& frame = resources_.cascades[cascadeIndex].history;
    const VkWriteDescriptorSetAccelerationStructureKHR structure{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR, nullptr, 1, &tlas};
    const std::array<VkDescriptorBufferInfo, 10> buffers{{
        {instances, 0, VK_WHOLE_SIZE}, {meshes, 0, VK_WHOLE_SIZE},
        {vertices, 0, VK_WHOLE_SIZE}, {indices, 0, VK_WHOLE_SIZE},
        {materials, 0, VK_WHOLE_SIZE},
        {frame.probeStates.handle(), 0, VK_WHOLE_SIZE},
        {frame.updateList.handle(), 0, VK_WHOLE_SIZE},
        {resources_.cascades[cascadeIndex].regionCache.handle(), 0, VK_WHOLE_SIZE},
        {resources_.cascades[cascadeIndex].cacheMapping.handle(), 0, VK_WHOLE_SIZE},
        {resources_.cascades[cascadeIndex].rayDirections[frameSlot].handle(), 0, VK_WHOLE_SIZE},
    }};
    const std::array<VkDescriptorImageInfo, 5> images{{
        {VK_NULL_HANDLE, frame.rayData.imageView(), VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, frame.irradiance.imageView(), VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, frame.distance.imageView(), VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, frame.probeData.imageView(), VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, frame.fixedRayData.imageView(), VK_IMAGE_LAYOUT_GENERAL},
    }};
    const std::array<VkDescriptorImageInfo, 3> sampledImages{{
        {frame.irradiance.sampler(), frame.irradiance.imageView(), VK_IMAGE_LAYOUT_GENERAL},
        {frame.distance.sampler(), frame.distance.imageView(), VK_IMAGE_LAYOUT_GENERAL},
        {frame.probeData.sampler(), frame.probeData.imageView(), VK_IMAGE_LAYOUT_GENERAL},
    }};
    std::array<VkWriteDescriptorSet, 19> writes{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &structure, sets_[cascadeIndex][frameSlot], 0, 0, 1,
                 VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR};
    for (std::uint32_t binding = 1; binding <= 4; ++binding)
        writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[cascadeIndex][frameSlot],
                           binding, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr,
                           &buffers[binding - 1]};
    for (std::uint32_t binding = 5; binding <= 7; ++binding)
        writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[cascadeIndex][frameSlot],
                           binding, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &images[binding - 5]};
    writes[8] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[cascadeIndex][frameSlot],
                 8, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &buffers[4]};
    for (std::uint32_t binding = 9; binding <= 10; ++binding)
        writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[cascadeIndex][frameSlot],
                           binding, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &images[binding - 6]};
    for (std::uint32_t binding = 11; binding <= 12; ++binding)
        writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[cascadeIndex][frameSlot],
                           binding, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &buffers[binding - 6]};
    for (std::uint32_t binding = 13; binding <= 15; ++binding)
        writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[cascadeIndex][frameSlot],
                           binding, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           &sampledImages[binding - 13]};
    for (std::uint32_t binding = 16; binding <= 17; ++binding)
        writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[cascadeIndex][frameSlot],
                           binding, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &buffers[binding - 9]};
    writes[18] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[cascadeIndex][frameSlot],
                  18, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &buffers[9]};
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

    const bool initialized = state.initialized;
    if (initialized) {
        // The previous graphics submission wrote the shared probe state. Make
        // it visible to this frame's schedule, validation and blend passes.
        computeResourceBarrier(commandBuffer,
            {frame.probeStates.handle(), frame.updateList.handle()});
    }
    const std::array startBarriers{
        imageBarrier(frame.rayData.image(), initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_GENERAL,
                     initialized ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
                     initialized ? VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT : 0,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT),
        imageBarrier(frame.irradiance.image(), initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_GENERAL,
                     initialized ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
                     initialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT),
        imageBarrier(frame.distance.image(), initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_GENERAL,
                     initialized ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
                     initialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT),
        imageBarrier(frame.fixedRayData.image(), initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_GENERAL,
                     initialized ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
                     initialized ? VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT : 0,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT),
        imageBarrier(frame.probeData.image(), initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_GENERAL,
                     initialized ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
                     initialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT),
    };
    emitImageBarriers(commandBuffer, startBarriers);
    const std::array sets{sceneSet, sets_[cascadeIndex][frameSlot]};
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_,
                            0, 2, sets.data(), 0, nullptr);
    const PushConstants push{
        {volume.origin[0], volume.origin[1], volume.origin[2], volume.probeSpacing},
        {state.scrollOffset[0], state.scrollOffset[1], state.scrollOffset[2],
         static_cast<std::int32_t>(volume.raysPerProbe)},
        {scrollDelta[0], scrollDelta[1], scrollDelta[2], static_cast<std::int32_t>(frameIndex)},
        {volume.maxRayDistance, initialized ? 1.0F : 0.0F, static_cast<float>(updateCount), 0.0F},
        {sceneChanged ? 1.0F : 0.0F, 0.0F, 0.0F, 0.0F}};
    cascadePushes[cascadeIndex] = push;
    vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    const bool scrolled = scrollDelta != std::array<std::int32_t, 3>{};
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, directionsPipeline_);
    vkCmdDispatch(commandBuffer, 1, 1, 1);
    if (!initialized || scrolled || sceneChanged) {
        auto& cache = resources_.cascades[cascadeIndex];
        // Protect both the old and new grids before choosing LRU victims. Their
        // union occupies at most 150 regions, below the 256-region capacity.
        if (sceneChanged) state.regions = {};
        const auto tick = ++state.cacheTick;
        std::array<std::array<std::int32_t, 3>, 4096> cells{};
        std::array<std::uint32_t, 4096> localIndices{};
        for (std::uint32_t side = 0; side < 2; ++side) {
            const auto& origin = side == 0 ? previousOrigin : newOriginCell;
            const auto& scroll = side == 0 ? previousScroll : state.scrollOffset;
            for (std::uint32_t probe = 0; probe < 2048; ++probe) {
                const std::array<std::int32_t, 3> physical{
                    static_cast<std::int32_t>(probe % 16),
                    static_cast<std::int32_t>((probe / 16) % 8),
                    static_cast<std::int32_t>(probe / 128)};
                std::array<std::int32_t, 3> local{};
                const auto index = side * 2048 + probe;
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    const auto world = origin[axis] + positiveModulo(physical[axis] - scroll[axis], counts[axis]);
                    local[axis] = positiveModulo(world, 4);
                    cells[index][axis] = (world - local[axis]) / 4;
                }
                localIndices[index] = static_cast<std::uint32_t>((local[2] * 4 + local[1]) * 4 + local[0]);
            }
        }
        const std::uint32_t first = initialized && !sceneChanged ? 0 : 2048;
        for (auto& region : state.regions)
            if (region.occupied && std::find(cells.begin() + first, cells.end(), region.cell) != cells.end())
                region.lastUsed = tick;
        VkBufferMemoryBarrier2 cacheBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        cacheBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        cacheBarrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        cacheBarrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        cacheBarrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        cacheBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        cacheBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        cacheBarrier.buffer = cache.regionCache.handle();
        cacheBarrier.size = VK_WHOLE_SIZE;
        VkBufferMemoryBarrier2 mappingReadBarrier = cacheBarrier;
        mappingReadBarrier.buffer = cache.cacheMapping.handle();
        mappingReadBarrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        const std::array beforeTransfer{cacheBarrier, mappingReadBarrier};
        VkDependencyInfo cacheDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        cacheDependency.bufferMemoryBarrierCount = static_cast<std::uint32_t>(beforeTransfer.size());
        cacheDependency.pBufferMemoryBarriers = beforeTransfer.data();
        vkCmdPipelineBarrier2(commandBuffer, &cacheDependency);
        std::array<std::uint32_t, 4096> mapping{};
        for (std::uint32_t i = first; i < mapping.size(); ++i) {
            auto found = std::find_if(state.regions.begin(), state.regions.end(),
                [&](const auto& region) { return region.occupied && region.cell == cells[i]; });
            if (found == state.regions.end()) {
                found = std::min_element(state.regions.begin(), state.regions.end(),
                    [](const auto& a, const auto& b) {
                        if (a.occupied != b.occupied) return !a.occupied;
                        return a.lastUsed < b.lastUsed;
                    });
                const auto slot = static_cast<VkDeviceSize>(found - state.regions.begin());
                vkCmdFillBuffer(commandBuffer, cache.regionCache.handle(), slot * cacheRegionBytes, cacheRegionBytes, 0);
                *found = {cells[i], tick, true};
            }
            const auto slot = static_cast<std::uint32_t>(found - state.regions.begin());
            mapping[i] = (slot * 64 + localIndices[i]) * cacheProbeWords;
        }
        vkCmdUpdateBuffer(commandBuffer, cache.cacheMapping.handle(), 0, sizeof(mapping), mapping.data());
        cacheBarrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        cacheBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        cacheBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        cacheBarrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        VkBufferMemoryBarrier2 mappingBarrier = cacheBarrier;
        mappingBarrier.buffer = cache.cacheMapping.handle();
        const std::array transferBarriers{cacheBarrier, mappingBarrier};
        cacheDependency.bufferMemoryBarrierCount = static_cast<std::uint32_t>(transferBarriers.size());
        cacheDependency.pBufferMemoryBarriers = transferBarriers.data();
        vkCmdPipelineBarrier2(commandBuffer, &cacheDependency);
        if (initialized && scrolled && !sceneChanged) {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, cacheStorePipeline_);
            vkCmdDispatch(commandBuffer, 2048, 1, 1);
            computeResourceBarrier(commandBuffer, {cache.regionCache.handle()});
        }
    }
    if (!initialized || scrolled) {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, scrollResetPipeline_);
        vkCmdDispatch(commandBuffer, 32, 1, 1);
        computeResourceBarrier(commandBuffer, {frame.probeStates.handle()},
            {frame.probeData.image(), frame.irradiance.image(), frame.distance.image()});
    }
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, schedulePipeline_);
    vkCmdDispatch(commandBuffer, 1, 1, 1);
    computeResourceBarrier(commandBuffer,
        {frame.updateList.handle(), frame.probeStates.handle(),
         resources_.cascades[cascadeIndex].rayDirections[frameSlot].handle()},
        {frame.probeData.image()});
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, validatePipeline_);
    vkCmdDispatch(commandBuffer, 4, (updateCount + 7) / 8, 1);
    computeResourceBarrier(commandBuffer, {}, {frame.fixedRayData.image()});
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, relocatePipeline_);
    vkCmdDispatch(commandBuffer, (updateCount + 63) / 64, 1, 1);
    computeResourceBarrier(commandBuffer, {frame.updateList.handle(), frame.probeStates.handle()},
        {frame.probeData.image()});
    auto revalidatePush = push;
    revalidatePush.distanceInitialized[3] = 1.0F;
    vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(revalidatePush), &revalidatePush);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, validatePipeline_);
    vkCmdDispatch(commandBuffer, 4, (updateCount + 7) / 8, 1);
    vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    computeResourceBarrier(commandBuffer, {}, {frame.fixedRayData.image()});
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, classifyPipeline_);
    vkCmdDispatch(commandBuffer, (updateCount + 63) / 64, 1, 1);
    // Recursive gathers see relocated probes as pending until their new rays
    // have replaced the atlas. The finish pass activates successful updates.
    computeResourceBarrier(commandBuffer, {frame.probeStates.handle()}, {frame.probeData.image()});
    }

    const auto bindCascade = [&](std::size_t cascadeIndex) {
        const std::array cascadeSets{sceneSet, sets_[cascadeIndex][frameSlot]};
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_,
                                0, 2, cascadeSets.data(), 0, nullptr);
        vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(PushConstants), &cascadePushes[cascadeIndex]);
    };
    // Trace all cascades against their previous atlas contents before any blend writes.
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, tracePipeline_);
    for (std::size_t cascadeIndex = 0; cascadeIndex < volumes_.size(); ++cascadeIndex) {
        bindCascade(cascadeIndex);
        vkCmdDispatch(commandBuffer, (volumes_[cascadeIndex].raysPerProbe - 32 + 7) / 8,
                      (cascadeUpdates[cascadeIndex] + 7) / 8, 1);
    }
    computeResourceBarrier(commandBuffer, {},
        {resources_.cascades[0].history.rayData.image(),
         resources_.cascades[1].history.rayData.image(),
         resources_.cascades[2].history.rayData.image()});

    // Both blend passes read ray data and write separate atlases. No barrier is
    // needed between their dispatches, including across different cascades.
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, irradiancePipeline_);
    for (std::size_t cascadeIndex = 0; cascadeIndex < volumes_.size(); ++cascadeIndex) {
        bindCascade(cascadeIndex);
        vkCmdDispatch(commandBuffer, 1, 1, cascadeUpdates[cascadeIndex]);
    }
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, distancePipeline_);
    for (std::size_t cascadeIndex = 0; cascadeIndex < volumes_.size(); ++cascadeIndex) {
        bindCascade(cascadeIndex);
        vkCmdDispatch(commandBuffer, 1, 1, cascadeUpdates[cascadeIndex]);
    }
    computeResourceBarrier(commandBuffer,
        {resources_.cascades[0].history.probeStates.handle(),
         resources_.cascades[1].history.probeStates.handle(),
         resources_.cascades[2].history.probeStates.handle()});

    // Finish reads each probe's acceptance from the irradiance pass.
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, finishPipeline_);
    for (std::size_t cascadeIndex = 0; cascadeIndex < volumes_.size(); ++cascadeIndex) {
        bindCascade(cascadeIndex);
        vkCmdDispatch(commandBuffer, (cascadeUpdates[cascadeIndex] + 63) / 64, 1, 1);
    }
    std::array<VkImageMemoryBarrier2, 9> finishBarriers{};
    for (std::size_t cascadeIndex = 0; cascadeIndex < volumes_.size(); ++cascadeIndex) {
        auto& frame = resources_.cascades[cascadeIndex].history;
        const auto toFragmentRead = [&](VkImage image) {
            return imageBarrier(image, VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        };
        finishBarriers[cascadeIndex * 3] = toFragmentRead(frame.irradiance.image());
        finishBarriers[cascadeIndex * 3 + 1] = toFragmentRead(frame.distance.image());
        finishBarriers[cascadeIndex * 3 + 2] = toFragmentRead(frame.probeData.image());
        cascadeStates_[cascadeIndex].initialized = true;
    }
    emitImageBarriers(commandBuffer, finishBarriers);
}

bool DDGISystem::ready() const noexcept {
    return std::all_of(cascadeStates_.begin(), cascadeStates_.end(),
        [](const auto& cascade) { return cascade.initialized; });
}

void DDGISystem::destroy() noexcept {
    if (device_) {
        if (tracePipeline_) vkDestroyPipeline(device_, tracePipeline_, nullptr);
        if (directionsPipeline_) vkDestroyPipeline(device_, directionsPipeline_, nullptr);
        if (schedulePipeline_) vkDestroyPipeline(device_, schedulePipeline_, nullptr);
        if (finishPipeline_) vkDestroyPipeline(device_, finishPipeline_, nullptr);
        if (validatePipeline_) vkDestroyPipeline(device_, validatePipeline_, nullptr);
        if (relocatePipeline_) vkDestroyPipeline(device_, relocatePipeline_, nullptr);
        if (classifyPipeline_) vkDestroyPipeline(device_, classifyPipeline_, nullptr);
        if (cacheStorePipeline_) vkDestroyPipeline(device_, cacheStorePipeline_, nullptr);
        if (scrollResetPipeline_) vkDestroyPipeline(device_, scrollResetPipeline_, nullptr);
        if (irradiancePipeline_) vkDestroyPipeline(device_, irradiancePipeline_, nullptr);
        if (distancePipeline_) vkDestroyPipeline(device_, distancePipeline_, nullptr);
        if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        if (pool_) vkDestroyDescriptorPool(device_, pool_, nullptr);
        if (layout_) vkDestroyDescriptorSetLayout(device_, layout_, nullptr);
    }
    for (auto& cascade : resources_.cascades) {
        auto& frame = cascade.history;
        cascade.regionCache.destroy();
        cascade.cacheMapping.destroy();
        for (auto& directions : cascade.rayDirections) directions.destroy();
        frame.rayData.destroy();
        frame.irradiance.destroy();
        frame.distance.destroy();
        frame.fixedRayData.destroy();
        frame.probeData.destroy();
        frame.probeStates.destroy();
        frame.updateList.destroy();
    }
    tracePipeline_ = irradiancePipeline_ = distancePipeline_ = VK_NULL_HANDLE;
    directionsPipeline_ = VK_NULL_HANDLE;
    schedulePipeline_ = finishPipeline_ = VK_NULL_HANDLE;
    validatePipeline_ = relocatePipeline_ = classifyPipeline_ = VK_NULL_HANDLE;
    cacheStorePipeline_ = scrollResetPipeline_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    sets_ = {};
    cascadeStates_ = {};
    sceneRevisions_ = {};
    sceneRevisionsInitialized_ = false;
    volumes_ = {};
    device_ = VK_NULL_HANDLE;
}
}
