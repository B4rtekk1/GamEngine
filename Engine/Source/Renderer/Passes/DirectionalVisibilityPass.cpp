#include "Engine/Renderer/Passes/DirectionalVisibilityPass.h"
#include "Engine/Renderer/shader_loader.h"

#include <array>
#include <stdexcept>

namespace Engine {
DirectionalVisibilityPass::~DirectionalVisibilityPass() { destroy(); }

void DirectionalVisibilityPass::create(const VkPhysicalDevice physical, const VkDevice device,
    const VkExtent2D extent, const VmaAllocator allocator, Assets::AssetManager& assets,
    const ShadowMap& shadowMap, const std::span<const VkBuffer> frameBuffers,
    const std::span<const VkBuffer> pageTables) {
    if (!device || !extent.width || !extent.height || frameBuffers.size() != sets_.size() ||
        pageTables.size() != sets_.size())
        throw std::invalid_argument("Directional visibility requires two frame buffers and page tables");
    destroy();
    device_ = device;
    extent_ = extent;
    try {
        for (auto& target : visibility_)
            target.create(physical, device_, extent_, allocator, VK_FILTER_NEAREST, VK_FORMAT_R32_SFLOAT, true);
        const std::array<VkDescriptorSetLayoutBinding, 8> bindings{{
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        }};
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &layout_) != VK_SUCCESS)
            throw std::runtime_error("Could not create directional visibility descriptor layout");
        const std::array<VkDescriptorPoolSize, 4> sizes{{
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 10},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
        }};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 2;
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
        poolInfo.pPoolSizes = sizes.data();
        if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &pool_) != VK_SUCCESS)
            throw std::runtime_error("Could not create directional visibility descriptor pool");
        const std::array<VkDescriptorSetLayout, 2> layouts{layout_, layout_};
        VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocateInfo.descriptorPool = pool_;
        allocateInfo.descriptorSetCount = 2;
        allocateInfo.pSetLayouts = layouts.data();
        if (vkAllocateDescriptorSets(device_, &allocateInfo, sets_.data()) != VK_SUCCESS)
            throw std::runtime_error("Could not allocate directional visibility descriptor sets");
        const std::array<VkDescriptorImageInfo, 3> shadowImages{{
            {shadowMap.sampler(), shadowMap.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {shadowMap.linearSampler(), shadowMap.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {shadowMap.depthSampler(), shadowMap.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        }};
        for (std::uint32_t frame = 0; frame < 2; ++frame) {
            const VkDescriptorBufferInfo pageInfo{pageTables[frame], 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo frameInfo{frameBuffers[frame], 0, VK_WHOLE_SIZE};
            const VkDescriptorImageInfo output{VK_NULL_HANDLE, visibility_[frame].imageView(), VK_IMAGE_LAYOUT_GENERAL};
            const std::array<VkWriteDescriptorSet, 6> writes{{
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &shadowImages[0]},
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &shadowImages[1]},
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &shadowImages[2]},
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 3, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &pageInfo},
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 6, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &frameInfo},
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 7, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &output},
            }};
            vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &layout_;
        if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS)
            throw std::runtime_error("Could not create directional visibility pipeline layout");
        const auto shader = Vkutil::loadShaderModule(device_, assets, "shaders/directional_visibility.spv");
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
            VK_SHADER_STAGE_COMPUTE_BIT, shader.get(), "main", nullptr};
        pipelineInfo.layout = pipelineLayout_;
        if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_) != VK_SUCCESS)
            throw std::runtime_error("Could not create directional visibility pipeline");
    } catch (...) { destroy(); throw; }
}

void DirectionalVisibilityPass::record(const VkCommandBuffer commandBuffer, const std::uint32_t frameSlot,
    const VkImageView depth, const VkSampler depthSampler,
    const VkImageView normals, const VkSampler normalSampler) {
    if (!pipeline_ || !depth || !normals || frameSlot >= sets_.size()) return;
    const VkDescriptorImageInfo depthInfo{depthSampler, depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo normalInfo{normalSampler, normals, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const std::array<VkWriteDescriptorSet, 2> writes{{
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frameSlot], 4, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frameSlot], 5, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &normalInfo},
    }};
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.image = visibility_[frameSlot].image();
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
        pipelineLayout_, 0, 1, &sets_[frameSlot], 0, nullptr);
    vkCmdDispatch(commandBuffer, (extent_.width + 7) / 8, (extent_.height + 7) / 8, 1);
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    // This image is consumed by RT contact shadow compute before the forward
    // fragment pass samples it.
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

void DirectionalVisibilityPass::destroy() noexcept {
    if (device_) {
        if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
        if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        if (pool_) vkDestroyDescriptorPool(device_, pool_, nullptr);
        if (layout_) vkDestroyDescriptorSetLayout(device_, layout_, nullptr);
    }
    for (auto& target : visibility_) target.destroy();
    pipeline_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    sets_ = {};
    extent_ = {};
    device_ = VK_NULL_HANDLE;
}
} // namespace Engine
