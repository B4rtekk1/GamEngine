#include "Engine/Renderer/Passes/VisibilityBufferPass.h"

#include "Engine/Renderer/Vulkan/PipelineCache.h"
#include "Engine/Renderer/shader_loader.h"

#include <array>
#include <stdexcept>

namespace Engine {
namespace {
constexpr std::uint32_t FramesInFlight = 2;

VkImageMemoryBarrier2 imageBarrier(const VkImage image, const VkPipelineStageFlags2 srcStage,
                                   const VkAccessFlags2 srcAccess, const VkImageLayout oldLayout,
                                   const VkPipelineStageFlags2 dstStage, const VkAccessFlags2 dstAccess,
                                   const VkImageLayout newLayout) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return barrier;
}

template <std::size_t Count> void submitBarriers(const VkCommandBuffer cmd, const std::array<VkImageMemoryBarrier2, Count>& barriers) {
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size());
    dependency.pImageMemoryBarriers = barriers.data();
    vkCmdPipelineBarrier2(cmd, &dependency);
}
} // namespace

VisibilityBufferPass::~VisibilityBufferPass() {
    destroy();
}

void VisibilityBufferPass::create(const VkPhysicalDevice physical, const VkDevice device, const VkExtent2D extent,
                                  const VmaAllocator allocator, Assets::AssetManager& assets,
                                  const VkDescriptorSetLayout sceneLayout, const VkDescriptorSetLayout meshletDrawLayout) {
    if (!device || !extent.width || !extent.height || !sceneLayout || !meshletDrawLayout)
        throw std::invalid_argument("The visibility buffer requires the forward layouts and an extent");
    destroy();
    device_ = device;
    extent_ = extent;
    try {
        ids_.create(physical, device_, extent_, allocator, VK_FILTER_NEAREST, IdFormat);
        constexpr VkShaderStageFlags Compute = VK_SHADER_STAGE_COMPUTE_BIT;
        // forward_pbr.slang (FORWARD_VISIBILITY_RESOLVE): ids, HDR, deferred surface.
        const std::array<VkDescriptorSetLayoutBinding, 3> bindings{{
            {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, Compute, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, Compute, nullptr},
            {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, Compute, nullptr},
        }};
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &layout_) != VK_SUCCESS)
            throw std::runtime_error("Could not create visibility resolve descriptor layout");
        const std::array<VkDescriptorPoolSize, 2> sizes{{
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, FramesInFlight},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, FramesInFlight * 2},
        }};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = FramesInFlight;
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
        poolInfo.pPoolSizes = sizes.data();
        if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &pool_) != VK_SUCCESS)
            throw std::runtime_error("Could not create visibility resolve descriptor pool");
        std::array<VkDescriptorSetLayout, FramesInFlight> layouts{};
        layouts.fill(layout_);
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = pool_;
        allocation.descriptorSetCount = FramesInFlight;
        allocation.pSetLayouts = layouts.data();
        if (vkAllocateDescriptorSets(device_, &allocation, sets_.data()) != VK_SUCCESS)
            throw std::runtime_error("Could not allocate visibility resolve descriptor sets");
        for (const VkDescriptorSet set : sets_) {
            const VkDescriptorImageInfo idsInfo{VK_NULL_HANDLE, ids_.imageView(),
                                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            const VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1,
                                             VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &idsInfo};
            vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        }
        const std::array<VkDescriptorSetLayout, 3> setLayouts{sceneLayout, meshletDrawLayout, layout_};
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = static_cast<std::uint32_t>(setLayouts.size());
        pipelineLayoutInfo.pSetLayouts = setLayouts.data();
        if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS)
            throw std::runtime_error("Could not create visibility resolve pipeline layout");
        const auto shader = Vkutil::loadShaderModule(device_, assets, "shaders/visibility_resolve.spv");
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
            throw std::runtime_error("Could not create visibility resolve pipeline");
    } catch (...) {
        destroy();
        throw;
    }
}

void VisibilityBufferPass::destroy() noexcept {
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
    ids_.destroy();
    pipeline_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    sets_ = {};
    extent_ = {};
    device_ = VK_NULL_HANDLE;
}

void VisibilityBufferPass::prepare(const VkCommandBuffer cmd) const {
    if (ids_.image() == VK_NULL_HANDLE)
        return;
    // The previous frame's resolve read the ids; the prepass clears them.
    submitBarriers(cmd, std::array{imageBarrier(ids_.image(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                                                VK_IMAGE_LAYOUT_UNDEFINED,
                                                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)});
}

void VisibilityBufferPass::record(const VkCommandBuffer cmd, const std::uint32_t frame, const VkDescriptorSet sceneSet,
                                  const VkDescriptorSet meshletDrawSet, const Targets& targets) {
    if (!pipeline_ || frame >= FramesInFlight || !sceneSet || !meshletDrawSet || !targets.hdr || !targets.surface)
        return;
    const VkDescriptorImageInfo hdrInfo{VK_NULL_HANDLE, targets.hdrView, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo surfaceInfo{VK_NULL_HANDLE, targets.surfaceView, VK_IMAGE_LAYOUT_GENERAL};
    const std::array<VkWriteDescriptorSet, 2> writes{{
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         &hdrInfo},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets_[frame], 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         &surfaceInfo},
    }};
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

    constexpr VkPipelineStageFlags2 Attachment = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    constexpr VkPipelineStageFlags2 Compute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    constexpr VkAccessFlags2 AttachmentWrite = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    constexpr VkAccessFlags2 StorageWrite = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    // The lighting pass cleared both targets and shaded the other slots;
    // the prepass wrote the ids.
    submitBarriers(cmd, std::array{
                            imageBarrier(ids_.image(), Attachment, AttachmentWrite,
                                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, Compute,
                                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
                            imageBarrier(targets.hdr, Attachment, AttachmentWrite,
                                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, Compute, StorageWrite,
                                         VK_IMAGE_LAYOUT_GENERAL),
                            imageBarrier(targets.surface, Attachment, AttachmentWrite,
                                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, Compute, StorageWrite,
                                         VK_IMAGE_LAYOUT_GENERAL),
                        });
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    const std::array sets{sceneSet, meshletDrawSet, sets_[frame]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0,
                            static_cast<std::uint32_t>(sets.size()), sets.data(), 0, nullptr);
    vkCmdDispatch(cmd, (extent_.width + 7) / 8, (extent_.height + 7) / 8, 1);
    // Back to attachments: the deferred local light pass expects them there
    // and reads both after its own attachment-to-storage barrier.
    constexpr VkAccessFlags2 Consumers = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | AttachmentWrite |
                                         VK_ACCESS_2_SHADER_STORAGE_READ_BIT | StorageWrite;
    submitBarriers(cmd, std::array{
                            imageBarrier(targets.hdr, Compute, StorageWrite, VK_IMAGE_LAYOUT_GENERAL,
                                         Attachment | Compute, Consumers, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
                            imageBarrier(targets.surface, Compute, StorageWrite, VK_IMAGE_LAYOUT_GENERAL,
                                         Attachment | Compute, Consumers, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
                        });
}
} // namespace Engine
