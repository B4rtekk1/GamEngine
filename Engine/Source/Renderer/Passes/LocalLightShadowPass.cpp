#include "Engine/Renderer/Passes/LocalLightShadowPass.h"

#include "Engine/Renderer/Vulkan/PipelineCache.h"
#include "Engine/Renderer/shader_loader.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>

namespace Engine {
namespace {
constexpr std::uint32_t FramesInFlight = 2;

struct PushConstants final {
    std::uint32_t maxRays;
    float normalBias;
    float minIrradiance;
    std::uint32_t convergedInterval;
    // Cluster scene partitioned TLAS (ray tracing pipeline only).
    std::uint64_t sceneAddress;
    std::uint32_t mode;
    std::uint32_t padding;
};
static_assert(sizeof(PushConstants) == 32, "matches LocalShadowSettings");

VkDeviceSize alignUp(const VkDeviceSize value, const VkDeviceSize alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

void transition(const VkCommandBuffer cmd, const VkImage image, const VkPipelineStageFlags2 srcStage,
                const VkAccessFlags2 srcAccess, const VkImageLayout oldLayout, const VkPipelineStageFlags2 dstStage,
                const VkAccessFlags2 dstAccess, const VkImageLayout newLayout) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}
} // namespace

LocalLightShadowPass::~LocalLightShadowPass() {
    destroy();
}

void LocalLightShadowPass::create(const VkPhysicalDevice physical, const VkDevice device, const VkExtent2D extent,
                                  const VmaAllocator allocator, Assets::AssetManager& assets,
                                  const std::span<const VkBuffer> ubos, const bool tracing,
                                  const bool clusterTracing) {
    if (!device || !extent.width || !extent.height || ubos.size() != FramesInFlight)
        throw std::invalid_argument("Local light shadow pass requires two frame UBOs and an extent");
    destroy();
    device_ = device;
    extent_ = extent;
    try {
        // One texel per 2x2 quad: the trace shares one sample per quad.
        // Integer targets are read texel-exact; never filtered.
        const VkExtent2D quads{(extent_.width + 1) / 2, (extent_.height + 1) / 2};
        for (auto& target : results_)
            target.create(physical, device_, quads, allocator, VK_FILTER_NEAREST, ResultFormat, true);
        resultDefined_ = {};
        if (!tracing && !clusterTracing)
            return;
        samples_.create(physical, device_, quads, allocator, VK_FILTER_NEAREST, SampleFormat, true);
        sampleWeights_.create(physical, device_, quads, allocator, VK_FILTER_NEAREST, SampleFormat, true);
        const VkShaderStageFlags stages =
            VK_SHADER_STAGE_COMPUTE_BIT | (clusterTracing ? VK_SHADER_STAGE_RAYGEN_BIT_KHR : VkShaderStageFlags{0});
        const std::array<VkDescriptorSetLayoutBinding, 11> bindings{{
            {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, stages, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, stages, nullptr},
            {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, stages, nullptr},
            {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, stages, nullptr},
            {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stages, nullptr},
            {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr},
            {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr},
            {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr},
            {8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, stages, nullptr},
            {9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr},
            {10, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, stages, nullptr},
        }};
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &layout_) != VK_SUCCESS)
            throw std::runtime_error("Could not create local light shadow descriptor layout");
        const std::array<VkDescriptorPoolSize, 5> sizes{{
            {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, FramesInFlight},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, FramesInFlight * 3},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, FramesInFlight * 2},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, FramesInFlight},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, FramesInFlight * 4},
        }};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = FramesInFlight;
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
        poolInfo.pPoolSizes = sizes.data();
        if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &pool_) != VK_SUCCESS)
            throw std::runtime_error("Could not create local light shadow descriptor pool");
        std::array<VkDescriptorSetLayout, FramesInFlight> layouts{};
        layouts.fill(layout_);
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = pool_;
        allocation.descriptorSetCount = FramesInFlight;
        allocation.pSetLayouts = layouts.data();
        if (vkAllocateDescriptorSets(device_, &allocation, sets_.data()) != VK_SUCCESS)
            throw std::runtime_error("Could not allocate local light shadow descriptor sets");
        for (std::uint32_t frame = 0; frame < FramesInFlight; ++frame) {
            const VkDescriptorImageInfo output{VK_NULL_HANDLE, samples_.imageView(), VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorBufferInfo ubo{ubos[frame], 0, VK_WHOLE_SIZE};
            // The trace reads the previous frame's denoised result.
            const HdrBuffer& previous = results_[(frame + 1) % FramesInFlight];
            const VkDescriptorImageInfo history{previous.sampler(), previous.imageView(),
                                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            const VkDescriptorImageInfo weights{VK_NULL_HANDLE, sampleWeights_.imageView(), VK_IMAGE_LAYOUT_GENERAL};
            const std::array<VkWriteDescriptorSet, 4> writes{{
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 3, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &output},
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 10, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &weights},
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 8, 0, 1,
                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &history},
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 4, 0, 1,
                 VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &ubo},
            }};
            vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
        createDenoisePipeline(assets, ubos);
        if (clusterTracing)
            createTracePipeline(physical, allocator, assets);
        if (!tracing)
            return;
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants)};
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &layout_;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &push;
        if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS)
            throw std::runtime_error("Could not create local light shadow pipeline layout");
        const auto shader = Vkutil::loadShaderModule(device_, assets, "shaders/local_light_shadow.spv");
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                              nullptr,
                              0,
                              VK_SHADER_STAGE_COMPUTE_BIT,
                              shader.get(),
                              "main",
                              nullptr};
        pipelineInfo.layout = pipelineLayout_;
        if (vkCreateComputePipelines(device_, PipelineCache::get(device_), 1, &pipelineInfo, nullptr, &pipeline_) !=
            VK_SUCCESS)
            throw std::runtime_error("Could not create local light shadow compute pipeline");
    } catch (...) {
        destroy();
        throw;
    }
}

void LocalLightShadowPass::createDenoisePipeline(Assets::AssetManager& assets, const std::span<const VkBuffer> ubos) {
    const std::array<VkDescriptorSetLayoutBinding, 6> bindings{{
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    }};
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &denoiseLayout_) != VK_SUCCESS)
        throw std::runtime_error("Could not create local light shadow denoise descriptor layout");
    const std::array<VkDescriptorPoolSize, 3> sizes{{
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, FramesInFlight * 2},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, FramesInFlight * 3},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, FramesInFlight},
    }};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = FramesInFlight;
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    poolInfo.pPoolSizes = sizes.data();
    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &denoisePool_) != VK_SUCCESS)
        throw std::runtime_error("Could not create local light shadow denoise descriptor pool");
    std::array<VkDescriptorSetLayout, FramesInFlight> layouts{};
    layouts.fill(denoiseLayout_);
    VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocation.descriptorPool = denoisePool_;
    allocation.descriptorSetCount = FramesInFlight;
    allocation.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device_, &allocation, denoiseSets_.data()) != VK_SUCCESS)
        throw std::runtime_error("Could not allocate local light shadow denoise descriptor sets");
    // Each slot writes its own result and accumulates over the other's.
    for (std::uint32_t frame = 0; frame < FramesInFlight; ++frame) {
        const VkDescriptorImageInfo samples{VK_NULL_HANDLE, samples_.imageView(), VK_IMAGE_LAYOUT_GENERAL};
        const HdrBuffer& previous = results_[(frame + 1) % FramesInFlight];
        const VkDescriptorImageInfo history{previous.sampler(), previous.imageView(),
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo output{VK_NULL_HANDLE, results_[frame].imageView(), VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorBufferInfo ubo{ubos[frame], 0, VK_WHOLE_SIZE};
        const std::array<VkWriteDescriptorSet, 4> writes{{
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, denoiseSets_[frame], 0, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &samples},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, denoiseSets_[frame], 3, 0, 1,
             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &history},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, denoiseSets_[frame], 4, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &output},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, denoiseSets_[frame], 5, 0, 1,
             VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &ubo},
        }};
        vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &denoiseLayout_;
    if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &denoisePipelineLayout_) != VK_SUCCESS)
        throw std::runtime_error("Could not create local light shadow denoise pipeline layout");
    const auto shader = Vkutil::loadShaderModule(device_, assets, "shaders/local_light_shadow_denoise.spv");
    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                          nullptr,
                          0,
                          VK_SHADER_STAGE_COMPUTE_BIT,
                          shader.get(),
                          "main",
                          nullptr};
    pipelineInfo.layout = denoisePipelineLayout_;
    if (vkCreateComputePipelines(device_, PipelineCache::get(device_), 1, &pipelineInfo, nullptr,
                                 &denoisePipeline_) != VK_SUCCESS)
        throw std::runtime_error("Could not create local light shadow denoise pipeline");
}

void LocalLightShadowPass::createTracePipeline(const VkPhysicalDevice physical, const VmaAllocator allocator,
                                               Assets::AssetManager& assets) {
    const VkPushConstantRange push{VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &layout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &tracePipelineLayout_) != VK_SUCCESS)
        throw std::runtime_error("Could not create local light shadow ray tracing layout");
    const auto shader = Vkutil::loadShaderModule(device_, assets, "shaders/local_light_shadow_rt.spv");
    const std::array<const char*, 3> names{"rayGen", "miss", "closestHit"};
    const std::array<VkShaderStageFlagBits, 3> stageBits{VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_MISS_BIT_KHR,
                                                         VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR};
    std::array<VkPipelineShaderStageCreateInfo, 3> stages{};
    for (std::size_t i = 0; i < stages.size(); ++i) {
        stages[i] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[i].stage = stageBits[i];
        stages[i].module = shader.get();
        stages[i].pName = names[i];
    }
    std::array<VkRayTracingShaderGroupCreateInfoKHR, 3> groups{};
    for (auto& group : groups) {
        group = {VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
        group.generalShader = group.closestHitShader = group.anyHitShader = group.intersectionShader =
            VK_SHADER_UNUSED_KHR;
    }
    groups[0].type = groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[0].generalShader = 0;
    groups[1].generalShader = 1;
    groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[2].closestHitShader = 2;
    // Cluster BLASes in the partitioned TLAS are visible only to pipelines
    // that opt in.
    VkRayTracingPipelineClusterAccelerationStructureCreateInfoNV clusters{
        VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CLUSTER_ACCELERATION_STRUCTURE_CREATE_INFO_NV};
    clusters.allowClusterAccelerationStructure = VK_TRUE;
    VkRayTracingPipelineCreateInfoKHR info{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    info.pNext = &clusters;
    info.stageCount = static_cast<std::uint32_t>(stages.size());
    info.pStages = stages.data();
    info.groupCount = static_cast<std::uint32_t>(groups.size());
    info.pGroups = groups.data();
    info.maxPipelineRayRecursionDepth = 1;
    info.layout = tracePipelineLayout_;
    const auto createPipelines = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(
        vkGetDeviceProcAddr(device_, "vkCreateRayTracingPipelinesKHR"));
    const auto getHandles = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(
        vkGetDeviceProcAddr(device_, "vkGetRayTracingShaderGroupHandlesKHR"));
    cmdTraceRays_ = reinterpret_cast<PFN_vkCmdTraceRaysKHR>(vkGetDeviceProcAddr(device_, "vkCmdTraceRaysKHR"));
    if (!createPipelines || !getHandles || !cmdTraceRays_ ||
        createPipelines(device_, VK_NULL_HANDLE, PipelineCache::get(device_), 1, &info, nullptr, &tracePipeline_) !=
            VK_SUCCESS)
        throw std::runtime_error("Could not create the local light shadow ray tracing pipeline");

    VkPhysicalDeviceRayTracingPipelinePropertiesKHR properties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties2.pNext = &properties;
    vkGetPhysicalDeviceProperties2(physical, &properties2);
    const VkDeviceSize handleSize = properties.shaderGroupHandleSize;
    const VkDeviceSize stride = alignUp(handleSize, properties.shaderGroupHandleAlignment);
    const VkDeviceSize regionSize = alignUp(stride, properties.shaderGroupBaseAlignment);
    std::vector<std::uint8_t> handles(handleSize * groups.size());
    if (getHandles(device_, tracePipeline_, 0, static_cast<std::uint32_t>(groups.size()), handles.size(),
                   handles.data()) != VK_SUCCESS)
        throw std::runtime_error("Could not read local light shadow shader group handles");
    const VkDeviceSize baseAlignment = properties.shaderGroupBaseAlignment;
    shaderBindingTable_.createHostVisible(physical, device_, regionSize * groups.size() + baseAlignment,
                                          VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR, allocator, true);
    const VkDeviceAddress base = alignUp(shaderBindingTable_.deviceAddress(), baseAlignment);
    const VkDeviceSize baseOffset = base - shaderBindingTable_.deviceAddress();
    for (std::size_t group = 0; group < groups.size(); ++group)
        shaderBindingTable_.update(handles.data() + group * handleSize, handleSize, baseOffset + group * regionSize);
    raygenRegion_ = {base, regionSize, regionSize};
    missRegion_ = {base + regionSize, stride, stride};
    hitRegion_ = {base + 2 * regionSize, stride, stride};
}

void LocalLightShadowPass::record(const VkCommandBuffer cmd, const std::uint32_t frame, const Inputs& inputs,
                                  const Settings& settings, const std::function<void()>& afterTrace) {
    const bool clusterPath = inputs.clusterScene != 0 && tracePipeline_ != VK_NULL_HANDLE;
    if ((!clusterPath && (!pipeline_ || !inputs.tlas)) || !inputs.depth || !inputs.normals || !inputs.clusterRanges ||
        !inputs.clusterIndices || !inputs.localLights || !inputs.tileLights || frame >= FramesInFlight)
        return;
    const VkWriteDescriptorSetAccelerationStructureKHR structureWrite{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR, nullptr, 1, &inputs.tlas};
    const VkDescriptorImageInfo depthInfo{inputs.depthSampler, inputs.depth,
                                          VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo normalInfo{inputs.normalSampler, inputs.normals,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo rangesInfo{inputs.clusterRanges, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo indicesInfo{inputs.clusterIndices, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo lightsInfo{inputs.localLights, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo tilesInfo{inputs.tileLights, 0, VK_WHOLE_SIZE};
    // The raygen build binds the scene by address and never reads binding 0
    // (the last write, left out there).
    const std::array<VkWriteDescriptorSet, 7> writes{{
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 9, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         nullptr, &tilesInfo},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 1, 0, 1,
         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 2, 0, 1,
         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &normalInfo},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 5, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         nullptr, &rangesInfo},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 6, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         nullptr, &indicesInfo},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 7, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         nullptr, &lightsInfo},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &structureWrite, sets_[frame], 0, 0, 1,
         VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR},
    }};
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(clusterPath ? writes.size() - 1 : writes.size()),
                           writes.data(), 0, nullptr);
    const VkPipelineStageFlags2 traceStage =
        clusterPath ? VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    // Both the trace (history reuse) and the denoiser read the previous
    // frame's result: give it a defined layout and make its writes visible.
    const std::uint32_t previous = (frame + 1) % FramesInFlight;
    if (!resultDefined_[previous])
        clearResult(cmd, previous);
    {
        VkImageMemoryBarrier2 history{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        history.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        history.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
        history.dstStageMask = traceStage | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        history.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        history.oldLayout = history.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        history.image = results_[previous].image();
        history.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &history;
        vkCmdPipelineBarrier2(cmd, &dependency);
    }
    // The previous frame's denoiser may still be reading the samples.
    transition(cmd, samples_.image(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_NONE,
               VK_IMAGE_LAYOUT_UNDEFINED, traceStage, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL);
    // Read by the previous frame's deferred local light pass.
    transition(cmd, sampleWeights_.image(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_NONE,
               VK_IMAGE_LAYOUT_UNDEFINED, traceStage, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL);
    // The acceleration structure was built or updated earlier on this queue
    // and the cluster lists by the clustered light culling dispatch.
    VkMemoryBarrier2 inputsBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    inputsBarrier.srcStageMask =
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    inputsBarrier.srcAccessMask =
        VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    inputsBarrier.dstStageMask = traceStage;
    inputsBarrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo inputsDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    inputsDependency.memoryBarrierCount = 1;
    inputsDependency.pMemoryBarriers = &inputsBarrier;
    vkCmdPipelineBarrier2(cmd, &inputsDependency);
    // Stochastic samples change every frame: no history reuse.
    const PushConstants constants{settings.maxRaysPerPixel,
                                  settings.normalBias,
                                  settings.minIrradiance,
                                  settings.stochastic ? 1U : std::max(settings.convergedRayInterval, 1U),
                                  clusterPath ? inputs.clusterScene : 0,
                                  settings.stochastic ? 1U : 0U,
                                  0U};
    // One invocation per 2x2 quad (see local_light_shadow.slang).
    const std::uint32_t quadsX = (extent_.width + 1) / 2;
    const std::uint32_t quadsY = (extent_.height + 1) / 2;
    if (clusterPath) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, tracePipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, tracePipelineLayout_, 0, 1, &sets_[frame],
                                0, nullptr);
        vkCmdPushConstants(cmd, tracePipelineLayout_, VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, sizeof(constants),
                           &constants);
        const VkStridedDeviceAddressRegionKHR callable{};
        cmdTraceRays_(cmd, &raygenRegion_, &missRegion_, &hitRegion_, &callable, quadsX, quadsY, 1);
    } else {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &sets_[frame], 0, nullptr);
        vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
        vkCmdDispatch(cmd, (quadsX + 7) / 8, (quadsY + 7) / 8, 1);
    }

    if (afterTrace)
        afterTrace();
    // Stochastic samples go to the deferred local light pass, which shades
    // and denoises the lighting itself.
    if (settings.stochastic)
        return;
    // Denoise into this slot's result over the previous frame's.
    const VkDescriptorImageInfo denoiseDepth{inputs.depthSampler, inputs.depth,
                                             VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo denoiseNormals{inputs.normalSampler, inputs.normals,
                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const std::array<VkWriteDescriptorSet, 2> denoiseWrites{{
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, denoiseSets_[frame], 1, 0, 1,
         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &denoiseDepth},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, denoiseSets_[frame], 2, 0, 1,
         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &denoiseNormals},
    }};
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(denoiseWrites.size()), denoiseWrites.data(), 0,
                           nullptr);
    std::array<VkImageMemoryBarrier2, 2> barriers{};
    for (auto& barrier : barriers) {
        barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    }
    // Samples: trace writes -> denoiser reads.
    barriers[0].srcStageMask = traceStage;
    barriers[0].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    barriers[0].oldLayout = barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[0].image = samples_.image();
    // Output: earlier frames' forward pass, trace and denoiser read it.
    barriers[1].srcStageMask =
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
        (tracePipeline_ != VK_NULL_HANDLE ? VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR : VkPipelineStageFlags2{0});
    barriers[1].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[1].image = results_[frame].image();
    VkDependencyInfo denoiseDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    denoiseDependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size());
    denoiseDependency.pImageMemoryBarriers = barriers.data();
    vkCmdPipelineBarrier2(cmd, &denoiseDependency);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, denoisePipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, denoisePipelineLayout_, 0, 1, &denoiseSets_[frame], 0,
                            nullptr);
    vkCmdDispatch(cmd, (quadsX + 7) / 8, (quadsY + 7) / 8, 1);
    transition(cmd, results_[frame].image(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    resultDefined_[frame] = true;
}

void LocalLightShadowPass::clear(const VkCommandBuffer cmd, const std::uint32_t frame) {
    if (frame < FramesInFlight)
        clearResult(cmd, frame);
}

void LocalLightShadowPass::clearResult(const VkCommandBuffer cmd, const std::uint32_t slot) {
    if (results_[slot].image() == VK_NULL_HANDLE)
        return;
    // Earlier frames' forward pass and denoiser may still read it.
    transition(cmd, results_[slot].image(),
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_NONE,
               VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    // All zero: no occlusion and no history.
    const VkClearColorValue zero{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, results_[slot].image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    transition(cmd, results_[slot].image(), VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    resultDefined_[slot] = true;
}

void LocalLightShadowPass::destroy() noexcept {
    if (device_) {
        if (pipeline_)
            vkDestroyPipeline(device_, pipeline_, nullptr);
        if (pipelineLayout_)
            vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        if (tracePipeline_)
            vkDestroyPipeline(device_, tracePipeline_, nullptr);
        if (tracePipelineLayout_)
            vkDestroyPipelineLayout(device_, tracePipelineLayout_, nullptr);
        if (denoisePipeline_)
            vkDestroyPipeline(device_, denoisePipeline_, nullptr);
        if (denoisePipelineLayout_)
            vkDestroyPipelineLayout(device_, denoisePipelineLayout_, nullptr);
        if (denoisePool_)
            vkDestroyDescriptorPool(device_, denoisePool_, nullptr);
        if (denoiseLayout_)
            vkDestroyDescriptorSetLayout(device_, denoiseLayout_, nullptr);
        if (pool_)
            vkDestroyDescriptorPool(device_, pool_, nullptr);
        if (layout_)
            vkDestroyDescriptorSetLayout(device_, layout_, nullptr);
    }
    for (auto& target : results_)
        target.destroy();
    samples_.destroy();
    sampleWeights_.destroy();
    resultDefined_ = {};
    denoisePipeline_ = VK_NULL_HANDLE;
    denoisePipelineLayout_ = VK_NULL_HANDLE;
    denoisePool_ = VK_NULL_HANDLE;
    denoiseLayout_ = VK_NULL_HANDLE;
    denoiseSets_ = {};
    pipeline_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    tracePipeline_ = VK_NULL_HANDLE;
    tracePipelineLayout_ = VK_NULL_HANDLE;
    shaderBindingTable_.destroy();
    cmdTraceRays_ = nullptr;
    pool_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    sets_ = {};
    extent_ = {};
    device_ = VK_NULL_HANDLE;
}
} // namespace Engine
