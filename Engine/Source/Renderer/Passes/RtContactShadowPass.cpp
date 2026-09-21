#include "Engine/Renderer/Passes/RtContactShadowPass.h"

#include "Engine/Renderer/shader_loader.h"

#include <array>
#include <stdexcept>

namespace Engine {
namespace {
    constexpr std::uint32_t FramesInFlight = 2;
}

RtContactShadowPass::~RtContactShadowPass() { destroy(); }

void RtContactShadowPass::create(const VkPhysicalDevice physical, const VkDevice device,
                                 const VkExtent2D extent, const VmaAllocator allocator,
                                 Assets::AssetManager& assets, const std::span<const VkBuffer> ubos) {
    if (!device || !extent.width || !extent.height || ubos.size() != FramesInFlight)
        throw std::invalid_argument("RT contact shadow pass requires two frame UBOs and an extent");
    destroy();
    device_ = device;
    extent_ = extent;
    try {
        // The forward pass samples this low-resolution mask at full-resolution.
        // Linear filtering is the inexpensive base upsample; edge-aware
        // reconstruction can be layered on top without changing this pass.
        visibility_.create(physical, device_, extent_, allocator, VK_FILTER_LINEAR, VK_FORMAT_R16_SFLOAT, true);
        const std::array<VkDescriptorSetLayoutBinding, 5> bindings{{
            {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        }};
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &layout_) != VK_SUCCESS)
            throw std::runtime_error("Could not create RT contact descriptor layout");
        const std::array<VkDescriptorPoolSize, 4> sizes{{
            {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, FramesInFlight},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, FramesInFlight * 2},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, FramesInFlight},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, FramesInFlight},
        }};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = FramesInFlight;
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
        poolInfo.pPoolSizes = sizes.data();
        if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &pool_) != VK_SUCCESS)
            throw std::runtime_error("Could not create RT contact descriptor pool");
        std::array<VkDescriptorSetLayout, FramesInFlight> layouts{};
        layouts.fill(layout_);
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = pool_;
        allocation.descriptorSetCount = FramesInFlight;
        allocation.pSetLayouts = layouts.data();
        if (vkAllocateDescriptorSets(device_, &allocation, sets_.data()) != VK_SUCCESS)
            throw std::runtime_error("Could not allocate RT contact descriptor sets");
        for (std::uint32_t frame = 0; frame < FramesInFlight; ++frame) {
            const VkDescriptorImageInfo output{VK_NULL_HANDLE, visibility_.imageView(), VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorBufferInfo ubo{ubos[frame], 0, VK_WHOLE_SIZE};
            const std::array<VkWriteDescriptorSet, 2> writes{{
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 3, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &output},
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 4, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &ubo},
            }};
            vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(float) * 2};
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &layout_;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &push;
        if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS)
            throw std::runtime_error("Could not create RT contact pipeline layout");
        const auto shader = Vkutil::loadShaderModule(device_, assets, "shaders/rt_contact_shadow.spv");
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                              VK_SHADER_STAGE_COMPUTE_BIT, shader.get(), "main", nullptr};
        pipelineInfo.layout = pipelineLayout_;
        if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_) != VK_SUCCESS)
            throw std::runtime_error("Could not create RT contact compute pipeline");
    } catch (...) { destroy(); throw; }
}

void RtContactShadowPass::record(const VkCommandBuffer cmd, const std::uint32_t frame,
                                 const VkAccelerationStructureKHR tlas, const VkImageView depth,
                                 const VkSampler depthSampler, const VkImageView normals,
                                 const VkSampler normalSampler, const RtContactShadowSettings& settings) {
    if (!pipeline_ || !tlas || !depth || !normals || frame >= FramesInFlight) return;
    const VkWriteDescriptorSetAccelerationStructureKHR structureWrite{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR, nullptr, 1, &tlas};
    const VkDescriptorImageInfo depthInfo{depthSampler, depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo normalInfo{normalSampler, normals, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const std::array<VkWriteDescriptorSet, 3> writes{{
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &structureWrite, sets_[frame], 0, 0, 1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &normalInfo},
    }};
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    VkImageMemoryBarrier2 toStorage{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toStorage.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    toStorage.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    toStorage.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    toStorage.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toStorage.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toStorage.image = visibility_.image();
    toStorage.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &toStorage;
    vkCmdPipelineBarrier2(cmd, &dependency);
    // The TLAS was built earlier in this command buffer.  RayQuery reads it
    // from the compute shader, so make the build writes visible first.
    VkMemoryBarrier2 asBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    asBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    asBarrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    asBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    asBarrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    VkDependencyInfo asDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    asDependency.memoryBarrierCount = 1;
    asDependency.pMemoryBarriers = &asBarrier;
    vkCmdPipelineBarrier2(cmd, &asDependency);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &sets_[frame], 0, nullptr);
    const float constants[] = {settings.maxDistance, settings.normalBias};
    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), constants);
    vkCmdDispatch(cmd, (extent_.width + 7) / 8, (extent_.height + 7) / 8, 1);
    VkImageMemoryBarrier2 toSample{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toSample.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    toSample.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    toSample.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    toSample.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    toSample.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toSample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toSample.image = visibility_.image();
    toSample.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dependency.pImageMemoryBarriers = &toSample;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

void RtContactShadowPass::destroy() noexcept {
    if (device_) {
        if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
        if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        if (pool_) vkDestroyDescriptorPool(device_, pool_, nullptr);
        if (layout_) vkDestroyDescriptorSetLayout(device_, layout_, nullptr);
    }
    visibility_.destroy();
    pipeline_ = VK_NULL_HANDLE; pipelineLayout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE; layout_ = VK_NULL_HANDLE; sets_ = {};
    extent_ = {}; device_ = VK_NULL_HANDLE;
}
} // namespace Engine
