#include "Engine/Renderer/Passes/SsrPass.h"

#include "Engine/Assets/AssetManager.h"
#include "Engine/Renderer/Vulkan/PipelineCache.h"
#include "Engine/Renderer/Vulkan/hdr_buffer.h"
#include "Engine/Renderer/shader_loader.h"

#include <algorithm>
#include <bit>
#include <stdexcept>
#include <string>

namespace Engine {
namespace {
// Levels glass_snapshot_downsample derives from its base level per dispatch.
constexpr std::uint32_t SnapshotFusedLevels = 4;
constexpr std::uint32_t MaxSnapshotMips = 8;

constexpr VkPipelineStageFlags2 ComputeStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr VkPipelineStageFlags2 ReaderStages = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | ComputeStage;

// Push constants of glass_snapshot_downsample.
struct SnapshotParams {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t pass;
    std::uint32_t levelCount;
};

[[nodiscard]] VkImageMemoryBarrier2 imageBarrier(const VkImage image, const VkPipelineStageFlags2 srcStage,
                                                 const VkAccessFlags2 srcAccess, const VkPipelineStageFlags2 dstStage,
                                                 const VkAccessFlags2 dstAccess, const VkImageLayout oldLayout,
                                                 const VkImageLayout newLayout, const std::uint32_t baseMip,
                                                 const std::uint32_t mipCount) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mipCount, 0, 1};
    return barrier;
}

void submit(const VkCommandBuffer commandBuffer, const VkImageMemoryBarrier2* barriers, const std::uint32_t count) {
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = count;
    dependency.pImageMemoryBarriers = barriers;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

[[nodiscard]] VkDescriptorSetLayout samplerStorageLayout(const VkDevice device, const std::uint32_t storageCount) {
    const std::array bindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
                                     nullptr},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, storageCount, VK_SHADER_STAGE_COMPUTE_BIT,
                                     nullptr},
    };
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    VkDescriptorSetLayout layout{VK_NULL_HANDLE};
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &layout) != VK_SUCCESS)
        throw std::runtime_error("Could not create an SSR set layout");
    return layout;
}

[[nodiscard]] VkPipelineLayout computeLayout(const VkDevice device, const VkDescriptorSetLayout setLayout,
                                             const std::uint32_t pushConstantBytes) {
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushConstantBytes};
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = 1;
    info.pSetLayouts = &setLayout;
    info.pushConstantRangeCount = pushConstantBytes != 0 ? 1U : 0U;
    info.pPushConstantRanges = pushConstantBytes != 0 ? &range : nullptr;
    VkPipelineLayout layout{VK_NULL_HANDLE};
    if (vkCreatePipelineLayout(device, &info, nullptr, &layout) != VK_SUCCESS)
        throw std::runtime_error("Could not create an SSR pipeline layout");
    return layout;
}

[[nodiscard]] VkPipeline computePipeline(const VkDevice device, Assets::AssetManager& assets,
                                         const VkPipelineLayout layout, const char* shader) {
    const auto module = Vkutil::loadShaderModule(device, assets, shader);
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module.get();
    info.stage.pName = "main";
    info.layout = layout;
    VkPipeline pipeline{VK_NULL_HANDLE};
    if (vkCreateComputePipelines(device, PipelineCache::get(device), 1, &info, nullptr, &pipeline) != VK_SUCCESS)
        throw std::runtime_error(std::string("Could not create SSR compute pipeline: ") + shader);
    return pipeline;
}

[[nodiscard]] std::uint32_t groups(const std::uint32_t size, const std::uint32_t groupSize) {
    return (size + groupSize - 1U) / groupSize;
}
} // namespace

SsrPass::~SsrPass() {
    destroy();
}

void SsrPass::create(const CreateInfo& info) {
    destroy();
    if (info.device == VK_NULL_HANDLE || info.assets == nullptr || info.extent.width == 0 ||
        info.extent.height == 0 || info.depthView == VK_NULL_HANDLE || info.colorView == VK_NULL_HANDLE)
        throw std::invalid_argument("SsrPass requires a device, assets, the scene depth and the HDR colour");
    info_ = info;
    sharingFamilies_.assign(info.sharingFamilies.begin(), info.sharingFamilies.end());
    info_.sharingFamilies = sharingFamilies_;
    device_ = info.device;
    extent_ = info.extent;
    try {
        createImages();
        createPipelines();
        createDescriptors();
    } catch (...) {
        destroy();
        throw;
    }
}

void SsrPass::createImages() {
    pyramid_.create(info_.physicalDevice, device_, extent_.width, extent_.height, info_.allocator, sharingFamilies_);
    pyramidLastLevel_ = std::min(pyramid_.mipCount() - 1, MaxPyramidLevel);

    snapshotMips_ = std::min<std::uint32_t>(MaxSnapshotMips, std::bit_width(std::max(extent_.width, extent_.height)));
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = HdrBuffer::Format;
    imageInfo.extent = {extent_.width, extent_.height, 1};
    imageInfo.mipLevels = snapshotMips_;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    // Written by glass_snapshot_downsample and the initial clear, sampled by Forward.
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(info_.allocator, &imageInfo, &allocation, &snapshot_, &snapshotMemory_, nullptr) != VK_SUCCESS)
        throw std::runtime_error("Could not allocate the SSR radiance snapshot");
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = snapshot_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = HdrBuffer::Format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, snapshotMips_, 0, 1};
    if (vkCreateImageView(device_, &viewInfo, nullptr, &snapshotView_) != VK_SUCCESS)
        throw std::runtime_error("Could not create the SSR radiance view");
    for (std::uint32_t mip = 0; mip < snapshotMips_; ++mip) {
        VkImageViewCreateInfo mipInfo = viewInfo;
        mipInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1};
        if (vkCreateImageView(device_, &mipInfo, nullptr, &snapshotMipViews_[mip]) != VK_SUCCESS)
            throw std::runtime_error("Could not create an SSR radiance level view");
    }
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = static_cast<float>(snapshotMips_ - 1);
    if (vkCreateSampler(device_, &samplerInfo, nullptr, &snapshotSampler_) != VK_SUCCESS)
        throw std::runtime_error("Could not create the SSR radiance sampler");
}

void SsrPass::createPipelines() {
    // hiz_initialize and glass_depth_reduce share one layout: a sampled
    // source and one storage destination, no push constants.
    reduceSetLayout_ = samplerStorageLayout(device_, 1);
    reducePipelineLayout_ = computeLayout(device_, reduceSetLayout_, 0);
    initializePipeline_ = computePipeline(device_, *info_.assets, reducePipelineLayout_, "shaders/hiz_initialize.spv");
    reducePipeline_ = computePipeline(device_, *info_.assets, reducePipelineLayout_, "shaders/glass_depth_reduce.spv");
    snapshotSetLayout_ = samplerStorageLayout(device_, static_cast<std::uint32_t>(snapshotMipViews_.size()));
    snapshotPipelineLayout_ = computeLayout(device_, snapshotSetLayout_, sizeof(SnapshotParams));
    snapshotPipeline_ =
        computePipeline(device_, *info_.assets, snapshotPipelineLayout_, "shaders/glass_snapshot_downsample.spv");
}

void SsrPass::createDescriptors() {
    const std::uint32_t levelCount = pyramidLastLevel_ + 1;
    const auto snapshotSlots = static_cast<std::uint32_t>(snapshotMipViews_.size());
    const std::array sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, levelCount + 1U},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, levelCount + snapshotSlots},
    };
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = levelCount + 1U;
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    poolInfo.pPoolSizes = sizes.data();
    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS)
        throw std::runtime_error("Could not create the SSR descriptor pool");

    std::vector<VkDescriptorSetLayout> layouts(levelCount, reduceSetLayout_);
    layouts.push_back(snapshotSetLayout_);
    std::vector<VkDescriptorSet> sets(layouts.size());
    VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocateInfo.descriptorPool = descriptorPool_;
    allocateInfo.descriptorSetCount = static_cast<std::uint32_t>(layouts.size());
    allocateInfo.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device_, &allocateInfo, sets.data()) != VK_SUCCESS)
        throw std::runtime_error("Could not allocate the SSR descriptor sets");
    levelSets_.assign(sets.begin(), sets.begin() + levelCount);
    snapshotSet_ = sets.back();

    std::vector<VkDescriptorImageInfo> images;
    images.reserve(2U * levelCount + 1U + snapshotSlots);
    std::vector<VkWriteDescriptorSet> writes;
    const auto add = [&](const VkSampler sampler, const VkImageView view, const VkImageLayout layout) {
        images.push_back({sampler, view, layout});
        return static_cast<std::uint32_t>(images.size() - 1);
    };
    const auto write = [&](const VkDescriptorSet set, const std::uint32_t binding, const VkDescriptorType type,
                           const std::uint32_t first, const std::uint32_t count) {
        writes.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, binding, 0, count, type,
                          images.data() + first, nullptr, nullptr});
    };
    for (std::uint32_t level = 0; level < levelCount; ++level) {
        write(levelSets_[level], 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
              level == 0 ? add(info_.depthSampler, info_.depthView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)
                         : add(pyramid_.sampler(), pyramid_.mipView(level - 1), VK_IMAGE_LAYOUT_GENERAL),
              1);
        write(levelSets_[level], 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
              add(VK_NULL_HANDLE, pyramid_.mipView(level), VK_IMAGE_LAYOUT_GENERAL), 1);
    }
    write(snapshotSet_, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          add(info_.colorSampler, info_.colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL), 1);
    std::uint32_t first = 0;
    // Slots past the last level are never written (the shader checks); they only need a valid view.
    for (std::uint32_t slot = 0; slot < snapshotSlots; ++slot) {
        const std::uint32_t index =
            add(VK_NULL_HANDLE, snapshotMipViews_[std::min(slot, snapshotMips_ - 1)], VK_IMAGE_LAYOUT_GENERAL);
        if (slot == 0)
            first = index;
    }
    write(snapshotSet_, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, first, snapshotSlots);
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void SsrPass::initialize(const VkCommandBuffer commandBuffer) {
    if (!created() || initialized_)
        return;
    std::array barriers{
        imageBarrier(snapshot_, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                     snapshotMips_),
        imageBarrier(pyramid_.image(), VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                     pyramid_.mipCount()),
    };
    submit(commandBuffer, barriers.data(), static_cast<std::uint32_t>(barriers.size()));
    const VkClearColorValue black{{0.0F, 0.0F, 0.0F, 0.0F}};
    const VkClearColorValue far{{1.0F, 1.0F, 1.0F, 1.0F}};
    const VkImageSubresourceRange snapshotRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, snapshotMips_, 0, 1};
    const VkImageSubresourceRange pyramidRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, pyramid_.mipCount(), 0, 1};
    vkCmdClearColorImage(commandBuffer, snapshot_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &snapshotRange);
    vkCmdClearColorImage(commandBuffer, pyramid_.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far, 1,
                         &pyramidRange);
    barriers[0] = imageBarrier(snapshot_, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               ReaderStages, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
                               snapshotMips_);
    barriers[1] = imageBarrier(pyramid_.image(), VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               ReaderStages, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
                               pyramid_.mipCount());
    submit(commandBuffer, barriers.data(), static_cast<std::uint32_t>(barriers.size()));
    initialized_ = true;
    radianceValid_ = false;
}

void SsrPass::recordPyramid(const VkCommandBuffer commandBuffer) {
    if (!created())
        return;
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, initializePipeline_);
    for (std::uint32_t level = 0; level <= pyramidLastLevel_; ++level) {
        if (level == 1)
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, reducePipeline_);
        if (level > 0) {
            const auto written = imageBarrier(pyramid_.image(), ComputeStage, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                              ComputeStage, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                              VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, level - 1, 1);
            submit(commandBuffer, &written, 1);
        }
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, reducePipelineLayout_, 0, 1,
                                &levelSets_[level], 0, nullptr);
        const VkExtent2D extent = pyramid_.mipExtent(level);
        vkCmdDispatch(commandBuffer, groups(extent.width, 8), groups(extent.height, 8), 1);
    }
}

void SsrPass::captureRadiance(const VkCommandBuffer commandBuffer) {
    if (!created() || !initialized_)
        return;
    // The lit colour was written as an attachment or from compute and is
    // already SHADER_READ_ONLY; this frame's Forward sampled the snapshot.
    const std::array before{
        imageBarrier(snapshot_, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | ComputeStage,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, ComputeStage, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, snapshotMips_),
    };
    VkMemoryBarrier2 colorWritten{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    colorWritten.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | ComputeStage;
    colorWritten.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    colorWritten.dstStageMask = ComputeStage;
    colorWritten.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &colorWritten;
    dependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(before.size());
    dependency.pImageMemoryBarriers = before.data();
    vkCmdPipelineBarrier2(commandBuffer, &dependency);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, snapshotPipeline_);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, snapshotPipelineLayout_, 0, 1,
                            &snapshotSet_, 0, nullptr);
    SnapshotParams params{extent_.width, extent_.height, 0, snapshotMips_};
    vkCmdPushConstants(commandBuffer, snapshotPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params),
                       &params);
    vkCmdDispatch(commandBuffer, groups(extent_.width, 16), groups(extent_.height, 16), 1);
    if (snapshotMips_ > SnapshotFusedLevels + 1) {
        // Levels 5.. continue from level 4, written above and read back here.
        const auto written = imageBarrier(snapshot_, ComputeStage, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, ComputeStage,
                                          VK_ACCESS_2_SHADER_STORAGE_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                                          VK_IMAGE_LAYOUT_GENERAL, SnapshotFusedLevels, 1);
        submit(commandBuffer, &written, 1);
        params.pass = 1;
        vkCmdPushConstants(commandBuffer, snapshotPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params),
                           &params);
        vkCmdDispatch(commandBuffer, groups(std::max(1U, extent_.width >> SnapshotFusedLevels), 16),
                      groups(std::max(1U, extent_.height >> SnapshotFusedLevels), 16), 1);
    }
    const auto after = imageBarrier(snapshot_, ComputeStage, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, ReaderStages,
                                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, snapshotMips_);
    // Also chains this compute read of the HDR target into the fragment
    // scope that later passes synchronise against before rewriting it.
    VkMemoryBarrier2 colorRead{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    colorRead.srcStageMask = ComputeStage;
    colorRead.srcAccessMask = VK_ACCESS_2_NONE;
    colorRead.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    colorRead.dstAccessMask = VK_ACCESS_2_NONE;
    dependency.pMemoryBarriers = &colorRead;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &after;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
    radianceValid_ = true;
}

void SsrPass::destroy() noexcept {
    if (device_ != VK_NULL_HANDLE) {
        if (descriptorPool_ != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        for (const VkPipeline pipeline : {initializePipeline_, reducePipeline_, snapshotPipeline_})
            if (pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device_, pipeline, nullptr);
        for (const VkPipelineLayout layout : {reducePipelineLayout_, snapshotPipelineLayout_})
            if (layout != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(device_, layout, nullptr);
        for (const VkDescriptorSetLayout layout : {reduceSetLayout_, snapshotSetLayout_})
            if (layout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(device_, layout, nullptr);
        if (snapshotSampler_ != VK_NULL_HANDLE)
            vkDestroySampler(device_, snapshotSampler_, nullptr);
        for (const VkImageView view : snapshotMipViews_)
            if (view != VK_NULL_HANDLE)
                vkDestroyImageView(device_, view, nullptr);
        if (snapshotView_ != VK_NULL_HANDLE)
            vkDestroyImageView(device_, snapshotView_, nullptr);
        if (snapshot_ != VK_NULL_HANDLE)
            vmaDestroyImage(info_.allocator, snapshot_, snapshotMemory_);
    }
    pyramid_.destroy();
    snapshot_ = VK_NULL_HANDLE;
    snapshotMemory_ = VK_NULL_HANDLE;
    snapshotView_ = VK_NULL_HANDLE;
    snapshotSampler_ = VK_NULL_HANDLE;
    snapshotMipViews_ = {};
    snapshotMips_ = 0;
    reduceSetLayout_ = VK_NULL_HANDLE;
    reducePipelineLayout_ = VK_NULL_HANDLE;
    initializePipeline_ = VK_NULL_HANDLE;
    reducePipeline_ = VK_NULL_HANDLE;
    levelSets_.clear();
    snapshotSetLayout_ = VK_NULL_HANDLE;
    snapshotPipelineLayout_ = VK_NULL_HANDLE;
    snapshotPipeline_ = VK_NULL_HANDLE;
    snapshotSet_ = VK_NULL_HANDLE;
    descriptorPool_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    initialized_ = false;
    radianceValid_ = false;
    pyramidLastLevel_ = 0;
}
} // namespace Engine
