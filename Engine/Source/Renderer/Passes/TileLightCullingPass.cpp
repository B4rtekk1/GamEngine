#include "Engine/Renderer/Passes/TileLightCullingPass.h"

#include "Engine/Renderer/Vulkan/PipelineCache.h"
#include "Engine/Renderer/Vulkan/renderer_types.h"
#include "Engine/Renderer/shader_loader.h"

#include <array>
#include <stdexcept>

namespace Engine {
namespace {
constexpr std::uint32_t FramesInFlight = 2;
} // namespace

TileLightCullingPass::~TileLightCullingPass() {
    destroy();
}

void TileLightCullingPass::create(const VkDevice device, const VkExtent2D extent, const VmaAllocator allocator,
                                  Assets::AssetManager& assets, const std::span<const VkBuffer> ubos,
                                  const bool culling) {
    if (!device || !extent.width || !extent.height || ubos.size() != FramesInFlight)
        throw std::invalid_argument("Tile light culling requires two frame UBOs and an extent");
    destroy();
    device_ = device;
    extent_ = extent;
    try {
        const VkDeviceSize tiles = static_cast<VkDeviceSize>((extent.width + TileLightPixelSize - 1) /
                                                             TileLightPixelSize) *
                                   ((extent.height + TileLightPixelSize - 1) / TileLightPixelSize);
        for (Buffer& lists : lists_)
            lists.createDeviceLocalEmpty(device_, tiles * TileLightStride * sizeof(std::uint32_t),
                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, allocator);
        if (!culling)
            return;
        const std::array<VkDescriptorSetLayoutBinding, 4> bindings{{
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        }};
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &layout_) != VK_SUCCESS)
            throw std::runtime_error("Could not create tile light culling descriptor layout");
        const std::array<VkDescriptorPoolSize, 3> sizes{{
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, FramesInFlight},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, FramesInFlight},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, FramesInFlight * 2},
        }};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = FramesInFlight;
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
        poolInfo.pPoolSizes = sizes.data();
        if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &pool_) != VK_SUCCESS)
            throw std::runtime_error("Could not create tile light culling descriptor pool");
        std::array<VkDescriptorSetLayout, FramesInFlight> layouts{};
        layouts.fill(layout_);
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = pool_;
        allocation.descriptorSetCount = FramesInFlight;
        allocation.pSetLayouts = layouts.data();
        if (vkAllocateDescriptorSets(device_, &allocation, sets_.data()) != VK_SUCCESS)
            throw std::runtime_error("Could not allocate tile light culling descriptor sets");
        for (std::uint32_t frame = 0; frame < FramesInFlight; ++frame) {
            const VkDescriptorBufferInfo ubo{ubos[frame], 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo output{lists_[frame].handle(), 0, VK_WHOLE_SIZE};
            const std::array<VkWriteDescriptorSet, 2> writes{{
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 1, 0, 1,
                 VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &ubo},
                {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 3, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &output},
            }};
            vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &layout_;
        if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS)
            throw std::runtime_error("Could not create tile light culling pipeline layout");
        const auto shader = Vkutil::loadShaderModule(device_, assets, "shaders/tile_light_culling.spv");
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
            throw std::runtime_error("Could not create tile light culling pipeline");
    } catch (...) {
        destroy();
        throw;
    }
}

void TileLightCullingPass::record(const VkCommandBuffer cmd, const std::uint32_t frame, const VkImageView depth,
                                  const VkSampler depthSampler, const VkBuffer localLights) {
    if (!pipeline_ || !depth || !localLights || frame >= FramesInFlight)
        return;
    const VkDescriptorImageInfo depthInfo{depthSampler, depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo lightsInfo{localLights, 0, VK_WHOLE_SIZE};
    const std::array<VkWriteDescriptorSet, 2> writes{{
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 0, 0, 1,
         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         nullptr, &lightsInfo},
    }};
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    // An earlier frame's forward pass read this slot's lists; the light
    // buffer was uploaded by transfer earlier on this queue.
    VkMemoryBarrier2 before{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    before.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    before.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    before.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    before.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo beforeDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    beforeDependency.memoryBarrierCount = 1;
    beforeDependency.pMemoryBarriers = &before;
    vkCmdPipelineBarrier2(cmd, &beforeDependency);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &sets_[frame], 0, nullptr);
    vkCmdDispatch(cmd, (extent_.width + TileLightPixelSize - 1) / TileLightPixelSize,
                  (extent_.height + TileLightPixelSize - 1) / TileLightPixelSize, 1);
    VkMemoryBarrier2 after{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    after.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    after.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    after.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    after.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo afterDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    afterDependency.memoryBarrierCount = 1;
    afterDependency.pMemoryBarriers = &after;
    vkCmdPipelineBarrier2(cmd, &afterDependency);
}

void TileLightCullingPass::destroy() noexcept {
    if (device_) {
        if (pipeline_)
            vkDestroyPipeline(device_, pipeline_, nullptr);
        if (pipelineLayout_)
            vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        if (pool_)
            vkDestroyDescriptorPool(device_, pool_, nullptr);
        if (layout_)
            vkDestroyDescriptorSetLayout(device_, layout_, nullptr);
    }
    for (Buffer& lists : lists_)
        lists.destroy();
    pipeline_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    sets_ = {};
    extent_ = {};
    device_ = VK_NULL_HANDLE;
}
} // namespace Engine
