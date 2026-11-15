#include "Engine/Renderer/Vulkan/TempImageLog.h" // TEMP-DEBUG
#include "Engine/Renderer/Passes/GlassPass.h"

#include "Engine/Assets/AssetManager.h"
#include "Engine/Core/Profiler.h"
#include "Engine/Renderer/Culling/IndexedIndirectDrawCount.h"
#include "Engine/Renderer/Geometry/GpuVertex.h"
#include "Engine/Renderer/Vulkan/GpuTimestampProfiler.h"
#include "Engine/Renderer/Vulkan/PipelineCache.h"
#include "Engine/Renderer/Vulkan/hdr_buffer.h"
#include "Engine/Renderer/shader_loader.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <stdexcept>
#include <string>

namespace Engine {
namespace {
// Coarser snapshot levels add nothing a frosted pane can show: by mip 7 a
// texel covers 128x128 pixels.
constexpr std::uint32_t MaxSnapshotMips = 8;
// Levels glass_snapshot_downsample derives from its base level per dispatch.
constexpr std::uint32_t SnapshotFusedLevels = 4;
// Deepest pyramid level the Hi-Z trace reads (GLASS_HIZ_MAX_MIP in
// Glass/glass_surface.slang); coarser levels are never built.
constexpr std::uint32_t MaxPyramidLevel = 7;
// glass_depth_downsample writes up to four levels (1..4) per dispatch. Level
// 0 is never written: the glass shaders read the scene depth for it.
constexpr std::uint32_t MaxFusedPyramidLevels = 4;
constexpr std::uint32_t PyramidFusedSlots = MaxFusedPyramidLevels;

[[nodiscard]] bool hasStencil(const VkFormat format) noexcept {
    return format == VK_FORMAT_D16_UNORM_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT ||
           format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

[[nodiscard]] VkImageMemoryBarrier2 imageBarrier(const VkImage image, const VkImageAspectFlags aspect,
                                                 const VkPipelineStageFlags2 srcStage,
                                                 const VkAccessFlags2 srcAccess,
                                                 const VkPipelineStageFlags2 dstStage,
                                                 const VkAccessFlags2 dstAccess, const VkImageLayout oldLayout,
                                                 const VkImageLayout newLayout, const std::uint32_t baseMip = 0,
                                                 const std::uint32_t mipCount = 1) {
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
    barrier.subresourceRange = {aspect, baseMip, mipCount, 0, 1};
    return barrier;
}

void submitBarriers(const VkCommandBuffer commandBuffer, const VkImageMemoryBarrier2* barriers,
                    const std::uint32_t count) {
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = count;
    dependency.pImageMemoryBarriers = barriers;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

template <std::size_t Count>
void submitBarriers(const VkCommandBuffer commandBuffer, const std::array<VkImageMemoryBarrier2, Count>& barriers) {
    submitBarriers(commandBuffer, barriers.data(), static_cast<std::uint32_t>(Count));
}

// Everything that may have produced the colour target before the glass pass:
// forward/water/particle rasterization and the virtual-water compute path.
constexpr VkPipelineStageFlags2 ColorProducerStages = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr VkAccessFlags2 ColorProducerAccess =
    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
constexpr VkPipelineStageFlags2 SampledReaderStages =
    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
} // namespace

GlassPass::~GlassPass() {
    destroy();
}

void GlassPass::create(const CreateInfo& info) {
    destroy();
    if (info.device == VK_NULL_HANDLE || info.assets == nullptr || info.sceneLayout == VK_NULL_HANDLE ||
        info.extent.width == 0 || info.extent.height == 0 || info.sceneDepthView == VK_NULL_HANDLE ||
        info.sceneDepthSampler == VK_NULL_HANDLE || info.depthFormat == VK_FORMAT_UNDEFINED)
        throw std::invalid_argument("GlassPass requires a device, assets, scene layout and a sampled scene depth");
    info_ = info;
    device_ = info.device;
    extent_ = info.extent;
    try {
        createImages();
        createPyramidPipelines();
        createSnapshotPipeline();
        createDescriptors();
        createGraphicsPipelines();
        createGate();
        planInputs();
    } catch (...) {
        destroy();
        throw;
    }
}

void GlassPass::createImages() {
    snapshotMips_ =
        std::min<std::uint32_t>(MaxSnapshotMips, std::bit_width(std::max(extent_.width, extent_.height)));
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = HdrBuffer::Format;
    imageInfo.extent = {extent_.width, extent_.height, 1};
    imageInfo.mipLevels = snapshotMips_;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    // Written by glass_snapshot_downsample, sampled by the glass shader.
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(info_.allocator, &imageInfo, &allocation, &snapshot_, &snapshotMemory_, nullptr) !=
        VK_SUCCESS)
        throw std::runtime_error("Could not allocate the glass scene snapshot");
    tempLogImage("Glass snapshot", snapshot_, imageInfo); // TEMP-DEBUG
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = snapshot_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = HdrBuffer::Format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, snapshotMips_, 0, 1};
    if (vkCreateImageView(device_, &viewInfo, nullptr, &snapshotView_) != VK_SUCCESS)
        throw std::runtime_error("Could not create the glass scene snapshot view");
    for (std::uint32_t mip = 0; mip < snapshotMips_; ++mip) {
        VkImageViewCreateInfo mipInfo = viewInfo;
        mipInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1};
        if (vkCreateImageView(device_, &mipInfo, nullptr, &snapshotMipViews_[mip]) != VK_SUCCESS)
            throw std::runtime_error("Could not create a glass scene snapshot level view");
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
        throw std::runtime_error("Could not create the glass scene snapshot sampler");

    pyramid_.create(info_.physicalDevice, device_, extent_.width, extent_.height, info_.allocator);
    pyramidLastLevel_ = std::min(pyramid_.mipCount() - 1, MaxPyramidLevel);
    // Fuse while each source level is even in both axes: a texel then covers
    // exactly 2x2 texels below it, the conservative MIN footprint.
    pyramidFusedLevels_ = 0;
    while (pyramidFusedLevels_ < std::min(MaxFusedPyramidLevels, pyramidLastLevel_)) {
        const VkExtent2D source = pyramid_.mipExtent(pyramidFusedLevels_);
        if (source.width < 2 || source.height < 2 || source.width % 2 != 0 || source.height % 2 != 0)
            break;
        ++pyramidFusedLevels_;
    }

    VkImageCreateInfo depthInfo = imageInfo;
    depthInfo.format = info_.depthFormat;
    depthInfo.mipLevels = 1;
    depthInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (vmaCreateImage(info_.allocator, &depthInfo, &allocation, &glassDepth_, &glassDepthMemory_, nullptr) !=
        VK_SUCCESS)
        throw std::runtime_error("Could not allocate the glass depth target");
    VkImageViewCreateInfo depthViewInfo = viewInfo;
    depthViewInfo.image = glassDepth_;
    depthViewInfo.format = info_.depthFormat;
    depthViewInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &depthViewInfo, nullptr, &glassDepthView_) != VK_SUCCESS)
        throw std::runtime_error("Could not create the glass depth view");
}

namespace {
[[nodiscard]] VkDescriptorSetLayout createSamplerStorageLayout(const VkDevice device, const std::uint32_t storageCount,
                                                               const char* what) {
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
        throw std::runtime_error(std::string("Could not create the ") + what + " set layout");
    return layout;
}

[[nodiscard]] VkPipelineLayout createComputeLayout(const VkDevice device, const VkDescriptorSetLayout setLayout,
                                                   const std::uint32_t pushConstantBytes, const char* what) {
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushConstantBytes};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &setLayout;
    pipelineLayoutInfo.pushConstantRangeCount = pushConstantBytes != 0 ? 1U : 0U;
    pipelineLayoutInfo.pPushConstantRanges = pushConstantBytes != 0 ? &range : nullptr;
    VkPipelineLayout layout{VK_NULL_HANDLE};
    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &layout) != VK_SUCCESS)
        throw std::runtime_error(std::string("Could not create the ") + what + " pipeline layout");
    return layout;
}

[[nodiscard]] VkPipeline createComputePipeline(const VkDevice device, Assets::AssetManager& assets,
                                               const VkPipelineLayout layout, const char* shader) {
    const auto module = Vkutil::loadShaderModule(device, assets, shader);
    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = module.get();
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = layout;
    VkPipeline pipeline{VK_NULL_HANDLE};
    if (vkCreateComputePipelines(device, PipelineCache::get(device), 1, &pipelineInfo, nullptr, &pipeline) !=
        VK_SUCCESS)
        throw std::runtime_error(std::string("Could not create glass compute pipeline: ") + shader);
    return pipeline;
}

// Push constants of glass_depth_downsample / glass_snapshot_downsample.
struct DownsampleParams {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t mode;  // depth: fused levels; snapshot: pass (0 or 1)
    std::uint32_t count; // depth: unused; snapshot: level count
};

[[nodiscard]] std::uint32_t groups(const std::uint32_t size, const std::uint32_t groupSize) {
    return (size + groupSize - 1U) / groupSize;
}
} // namespace

void GlassPass::createPyramidPipelines() {
    pyramidFusedSetLayout_ = createSamplerStorageLayout(device_, PyramidFusedSlots, "glass depth downsample");
    pyramidFusedPipelineLayout_ =
        createComputeLayout(device_, pyramidFusedSetLayout_, sizeof(DownsampleParams), "glass depth downsample");
    pyramidFusedPipeline_ = createComputePipeline(device_, *info_.assets, pyramidFusedPipelineLayout_,
                                                  "shaders/glass_depth_downsample.spv");
    pyramidSetLayout_ = createSamplerStorageLayout(device_, 1, "glass depth pyramid");
    pyramidPipelineLayout_ = createComputeLayout(device_, pyramidSetLayout_, 0, "glass depth pyramid");
    pyramidReducePipeline_ = createComputePipeline(device_, *info_.assets, pyramidPipelineLayout_,
                                                   "shaders/glass_depth_reduce.spv");
}

void GlassPass::createSnapshotPipeline() {
    snapshotSetLayout_ =
        createSamplerStorageLayout(device_, static_cast<std::uint32_t>(snapshotMipViews_.size()), "glass snapshot");
    snapshotPipelineLayout_ =
        createComputeLayout(device_, snapshotSetLayout_, sizeof(DownsampleParams), "glass snapshot");
    snapshotPipeline_ = createComputePipeline(device_, *info_.assets, snapshotPipelineLayout_,
                                              "shaders/glass_snapshot_downsample.spv");
}

void GlassPass::createDescriptors() {
    const std::uint32_t reduceSets = pyramidLastLevel_ - pyramidFusedLevels_;
    const auto snapshotSlots = static_cast<std::uint32_t>(snapshotMipViews_.size());
    const std::array sizes{
        // Glass set (3), fused pyramid, snapshot downsample, one per reduce.
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5U + reduceSets},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, PyramidFusedSlots + snapshotSlots + reduceSets},
        // The gate (createGate).
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1U},
    };
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 4U + reduceSets;
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    poolInfo.pPoolSizes = sizes.data();
    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS)
        throw std::runtime_error("Could not create the glass descriptor pool");

    const std::array bindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                     nullptr},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                     nullptr},
        // Scene depth: pyramid level 0 for the trace and the depth seed.
        VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                     nullptr},
    };
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &glassSetLayout_) != VK_SUCCESS)
        throw std::runtime_error("Could not create the glass set layout");

    std::vector<VkDescriptorSetLayout> layouts{glassSetLayout_, pyramidFusedSetLayout_, snapshotSetLayout_};
    layouts.insert(layouts.end(), reduceSets, pyramidSetLayout_);
    std::vector<VkDescriptorSet> sets(layouts.size());
    VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocateInfo.descriptorPool = descriptorPool_;
    allocateInfo.descriptorSetCount = static_cast<std::uint32_t>(layouts.size());
    allocateInfo.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device_, &allocateInfo, sets.data()) != VK_SUCCESS)
        throw std::runtime_error("Could not allocate the glass descriptor sets");
    glassSet_ = sets[0];
    pyramidFusedSet_ = sets[1];
    snapshotSet_ = sets[2];
    pyramidReduceSets_.assign(sets.begin() + 3, sets.end());

    // Image infos must outlive the vkUpdateDescriptorSets call below.
    std::vector<VkDescriptorImageInfo> images;
    images.reserve(4U + PyramidFusedSlots + snapshotSlots + 2U * reduceSets);
    std::vector<VkWriteDescriptorSet> writes;
    const auto write = [&](const VkDescriptorSet set, const std::uint32_t binding, const VkDescriptorType type,
                           const std::uint32_t first, const std::uint32_t count) {
        writes.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, binding, 0, count, type,
                          images.data() + first, nullptr, nullptr});
    };
    const auto add = [&](const VkSampler sampler, const VkImageView view, const VkImageLayout layout) {
        images.push_back({sampler, view, layout});
        return static_cast<std::uint32_t>(images.size() - 1);
    };

    write(glassSet_, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          add(snapshotSampler_, snapshotView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL), 1);
    write(glassSet_, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          add(pyramid_.sampler(), pyramid_.fullView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL), 1);
    const std::uint32_t sceneDepth =
        add(info_.sceneDepthSampler, info_.sceneDepthView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    write(glassSet_, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sceneDepth, 1);

    write(pyramidFusedSet_, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sceneDepth, 1);
    // Slot i is level i + 1. Slots past the fused levels are never written
    // (the shader checks); they only need a valid view.
    const std::uint32_t lastFusedView = std::max(pyramidFusedLevels_, std::min(1U, pyramid_.mipCount() - 1));
    std::uint32_t first = 0;
    for (std::uint32_t slot = 0; slot < PyramidFusedSlots; ++slot) {
        const std::uint32_t index =
            add(VK_NULL_HANDLE, pyramid_.mipView(std::min(slot + 1, lastFusedView)), VK_IMAGE_LAYOUT_GENERAL);
        if (slot == 0)
            first = index;
    }
    write(pyramidFusedSet_, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, first, PyramidFusedSlots);

    for (std::uint32_t i = 0; i < reduceSets; ++i) {
        const std::uint32_t level = pyramidFusedLevels_ + 1 + i;
        // Level 1 without fused levels reduces the scene depth itself.
        write(pyramidReduceSets_[i], 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
              level == 1 ? sceneDepth : add(pyramid_.sampler(), pyramid_.mipView(level - 1), VK_IMAGE_LAYOUT_GENERAL),
              1);
        write(pyramidReduceSets_[i], 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
              add(VK_NULL_HANDLE, pyramid_.mipView(level), VK_IMAGE_LAYOUT_GENERAL), 1);
    }

    // Binding 0 (the HDR colour, sampled in place) arrives with record().
    for (std::uint32_t slot = 0; slot < snapshotSlots; ++slot) {
        const std::uint32_t index = add(VK_NULL_HANDLE, snapshotMipViews_[std::min(slot, snapshotMips_ - 1)],
                                        VK_IMAGE_LAYOUT_GENERAL);
        if (slot == 0)
            first = index;
    }
    write(snapshotSet_, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, first, snapshotSlots);
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

namespace {
// glass_inputs_gate: the two copied draw counts, then one four-word entry
// per indirect command; at most the seed draw and ten input dispatches.
constexpr std::uint32_t GateMaxEntries = 12;
constexpr VkDeviceSize GateEntryBytes = 4 * sizeof(std::uint32_t);
constexpr VkDeviceSize GateHeaderBytes = 4 * sizeof(std::uint32_t);

struct GateConstants {
    std::uint32_t entries;
    std::array<std::uint32_t, 3> padding;
    std::array<std::uint32_t, 2 * GateMaxEntries> groups;
};
} // namespace

void GlassPass::createGate() {
    gate_.createDeviceLocalEmpty(device_, GateHeaderBytes + GateEntryBytes * GateMaxEntries,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 info_.allocator);
    const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
                                               nullptr};
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &gateSetLayout_) != VK_SUCCESS)
        throw std::runtime_error("Could not create the glass gate set layout");
    gatePipelineLayout_ = createComputeLayout(device_, gateSetLayout_, sizeof(GateConstants), "glass gate");
    gatePipeline_ = createComputePipeline(device_, *info_.assets, gatePipelineLayout_, "shaders/glass_inputs_gate.spv");
    VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocateInfo.descriptorPool = descriptorPool_;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &gateSetLayout_;
    if (vkAllocateDescriptorSets(device_, &allocateInfo, &gateSet_) != VK_SUCCESS)
        throw std::runtime_error("Could not allocate the glass gate set");
    const VkDescriptorBufferInfo bufferInfo{gate_.handle(), 0, VK_WHOLE_SIZE};
    const VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                     nullptr,
                                     gateSet_,
                                     0,
                                     0,
                                     1,
                                     VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                     nullptr,
                                     &bufferInfo,
                                     nullptr};
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

// The commands recordInputs() issues, in its order: the extents fix them.
void GlassPass::planInputs() {
    inputGroups_.clear();
    // The depth seed: one full-screen triangle.
    inputGroups_.push_back({3, 1});
    const std::array<std::uint32_t, 2> full{groups(extent_.width, 16), groups(extent_.height, 16)};
    if (pyramidFusedLevels_ != 0)
        inputGroups_.push_back(full);
    inputGroups_.push_back(full);
    bool snapshotPending = snapshotMips_ > SnapshotFusedLevels + 1;
    const std::array<std::uint32_t, 2> tail{groups(std::max(1U, extent_.width >> SnapshotFusedLevels), 16),
                                            groups(std::max(1U, extent_.height >> SnapshotFusedLevels), 16)};
    for (std::uint32_t level = pyramidFusedLevels_ + 1; level <= pyramidLastLevel_; ++level) {
        const VkExtent2D extent = pyramid_.mipExtent(level);
        inputGroups_.push_back({groups(extent.width, 8), groups(extent.height, 8)});
        if (snapshotPending)
            inputGroups_.push_back(tail);
        snapshotPending = false;
    }
    if (snapshotPending)
        inputGroups_.push_back(tail);
    if (inputGroups_.size() > GateMaxEntries)
        throw std::logic_error("GlassPass plans more indirect commands than its gate holds");
}

// Copies the glass slot's draw count of both streams into the gate and
// writes this frame's indirect commands, zeroed when both counts are zero.
void GlassPass::recordGate(const VkCommandBuffer commandBuffer, const RecordInfo& info) const {
    // Culling wrote the counts, and the previous frame's commands read the
    // gate; the copies then rewrite it.
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
    const std::array streams{info.singleSided, info.twoSided};
    for (std::uint32_t stream = 0; stream < streams.size(); ++stream) {
        const VkDeviceSize word = stream * sizeof(std::uint32_t);
        if (streams[stream] == nullptr || !streams[stream]->valid()) {
            vkCmdFillBuffer(commandBuffer, gate_.handle(), word, sizeof(std::uint32_t), 0);
            continue;
        }
        const VkBufferCopy copy{streams[stream]->countOffset() + info.countOffset, word, sizeof(std::uint32_t)};
        vkCmdCopyBuffer(commandBuffer, streams[stream]->countBuffer(), gate_.handle(), 1, &copy);
    }
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);

    GateConstants constants{};
    constants.entries = static_cast<std::uint32_t>(inputGroups_.size());
    for (std::size_t entry = 0; entry < inputGroups_.size(); ++entry) {
        constants.groups[2 * entry] = inputGroups_[entry][0];
        constants.groups[2 * entry + 1] = inputGroups_[entry][1];
    }
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, gatePipeline_);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, gatePipelineLayout_, 0, 1, &gateSet_, 0,
                            nullptr);
    vkCmdPushConstants(commandBuffer, gatePipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                       &constants);
    vkCmdDispatch(commandBuffer, 1, 1, 1);

    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

// The next planned input dispatch, from the gate.
void GlassPass::dispatchInput(const VkCommandBuffer commandBuffer, std::uint32_t& entry, const std::uint32_t x,
                              const std::uint32_t y) const {
    if (entry >= inputGroups_.size() || inputGroups_[entry][0] != x || inputGroups_[entry][1] != y)
        throw std::logic_error("GlassPass recorded an input dispatch its plan does not hold");
    vkCmdDispatchIndirect(commandBuffer, gate_.handle(), GateHeaderBytes + GateEntryBytes * entry);
    ++entry;
}

void GlassPass::createGraphicsPipelines() {
    GraphicsPipelineOptions options{};
    options.colorFormat = HdrBuffer::Format;
    options.dynamicRendering = true;
    options.depthFormat = info_.depthFormat;
    options.samples = VK_SAMPLE_COUNT_1_BIT;
    options.colorLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    options.depthLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    options.colorInitialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    options.colorFinalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    options.depthInitialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    options.depthFinalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    // Nearest glass surface wins; the shader composes the full colour.
    options.depthWriteEnable = VK_TRUE;
    options.depthTestEnable = VK_TRUE;
    // The target holds the opaque depth (see depthSeedPipeline_): a pane in
    // front of or on an opaque surface passes, as the shader's own test.
    options.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    options.alphaBlendEnable = VK_FALSE;
    options.shader = "shaders/forward_glass.spv";
    options.assetManager = info_.assets;
    options.cullMode = VK_CULL_MODE_BACK_BIT;
    options.descriptorSetLayouts = {info_.sceneLayout, glassSetLayout_};
    options.vertexBindings = {{0, sizeof(GpuVertex), VK_VERTEX_INPUT_RATE_VERTEX}};
    options.vertexAttributes = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, px)},
        {1, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(GpuVertex, color)},
        {2, 0, VK_FORMAT_R16G16_SFLOAT, offsetof(GpuVertex, texCoord)},
        {4, 0, VK_FORMAT_R16G16_SFLOAT, offsetof(GpuVertex, texCoord1)},
        {3, 0, VK_FORMAT_R32_UINT, offsetof(GpuVertex, tangentFrame)},
        {8, 0, VK_FORMAT_R32_UINT, offsetof(GpuVertex, materialIndex)},
    };
    singleSidedPipeline_.create(device_, options);
    options.cullMode = VK_CULL_MODE_NONE;
    twoSidedPipeline_.create(device_, options);

    GraphicsPipelineOptions seed = options;
    seed.shader = "shaders/glass_depth_seed.spv";
    seed.vertexBindings.clear();
    seed.vertexAttributes.clear();
    seed.colorWriteMask = 0;
    seed.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    depthSeedPipeline_.create(device_, seed);
}

void GlassPass::destroy() noexcept {
    depthSeedPipeline_.destroy();
    singleSidedPipeline_.destroy();
    twoSidedPipeline_.destroy();
    if (device_ != VK_NULL_HANDLE) {
        if (descriptorPool_ != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        for (const VkPipeline pipeline :
             {pyramidFusedPipeline_, pyramidReducePipeline_, snapshotPipeline_, gatePipeline_})
            if (pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device_, pipeline, nullptr);
        for (const VkPipelineLayout layout :
             {pyramidFusedPipelineLayout_, pyramidPipelineLayout_, snapshotPipelineLayout_, gatePipelineLayout_})
            if (layout != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(device_, layout, nullptr);
        for (const VkDescriptorSetLayout layout :
             {glassSetLayout_, pyramidFusedSetLayout_, pyramidSetLayout_, snapshotSetLayout_, gateSetLayout_})
            if (layout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(device_, layout, nullptr);
        if (snapshotSampler_ != VK_NULL_HANDLE)
            vkDestroySampler(device_, snapshotSampler_, nullptr);
        if (snapshotView_ != VK_NULL_HANDLE)
            vkDestroyImageView(device_, snapshotView_, nullptr);
        for (const VkImageView view : snapshotMipViews_)
            if (view != VK_NULL_HANDLE)
                vkDestroyImageView(device_, view, nullptr);
        if (glassDepthView_ != VK_NULL_HANDLE)
            vkDestroyImageView(device_, glassDepthView_, nullptr);
    }
    if (info_.allocator != VK_NULL_HANDLE) {
        if (snapshot_ != VK_NULL_HANDLE)
            vmaDestroyImage(info_.allocator, snapshot_, snapshotMemory_);
        if (glassDepth_ != VK_NULL_HANDLE)
            vmaDestroyImage(info_.allocator, glassDepth_, glassDepthMemory_);
    }
    pyramid_.destroy();
    gate_.destroy();
    inputGroups_.clear();
    gatePipeline_ = VK_NULL_HANDLE;
    gatePipelineLayout_ = VK_NULL_HANDLE;
    gateSetLayout_ = VK_NULL_HANDLE;
    gateSet_ = VK_NULL_HANDLE;
    descriptorPool_ = VK_NULL_HANDLE;
    glassSetLayout_ = VK_NULL_HANDLE;
    glassSet_ = pyramidFusedSet_ = snapshotSet_ = VK_NULL_HANDLE;
    pyramidReduceSets_.clear();
    pyramidFusedPipeline_ = pyramidReducePipeline_ = snapshotPipeline_ = VK_NULL_HANDLE;
    pyramidFusedPipelineLayout_ = pyramidPipelineLayout_ = snapshotPipelineLayout_ = VK_NULL_HANDLE;
    pyramidFusedSetLayout_ = pyramidSetLayout_ = snapshotSetLayout_ = VK_NULL_HANDLE;
    pyramidFusedLevels_ = pyramidLastLevel_ = 0;
    snapshotSampler_ = VK_NULL_HANDLE;
    snapshotView_ = VK_NULL_HANDLE;
    snapshotMipViews_.fill(VK_NULL_HANDLE);
    snapshotSourceView_ = VK_NULL_HANDLE;
    glassDepthView_ = VK_NULL_HANDLE;
    snapshot_ = glassDepth_ = VK_NULL_HANDLE;
    snapshotMemory_ = glassDepthMemory_ = VK_NULL_HANDLE;
    snapshotMips_ = 0;
    snapshotInitialized_ = pyramidInitialized_ = false;
    device_ = VK_NULL_HANDLE;
    extent_ = {};
    info_ = {};
}


void GlassPass::recordInputs(const VkCommandBuffer commandBuffer, const VkImage color, const VkImage sceneDepth) {
    const VkImageAspectFlags depthAspect =
        VK_IMAGE_ASPECT_DEPTH_BIT | (hasStencil(info_.depthFormat) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0U);
    // The previous frame sampled the pyramid and the snapshot; both are
    // rewritten. The colour and the depth keep their layouts: compute
    // samples them in place.
    const std::array before{
        imageBarrier(sceneDepth, depthAspect,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL),
        imageBarrier(pyramid_.image(), VK_IMAGE_ASPECT_COLOR_BIT,
                     pyramidInitialized_ ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
                     pyramidInitialized_ ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : VK_ACCESS_2_NONE,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, pyramid_.mipCount()),
        imageBarrier(color, VK_IMAGE_ASPECT_COLOR_BIT, ColorProducerStages, ColorProducerAccess,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        imageBarrier(snapshot_, VK_IMAGE_ASPECT_COLOR_BIT,
                     snapshotInitialized_ ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
                     snapshotInitialized_ ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : VK_ACCESS_2_NONE,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, snapshotMips_),
    };
    submitBarriers(commandBuffer, before);
    // Entry 0 is the depth seed draw; the dispatches follow in plan order.
    std::uint32_t entry = 1;

    // The pyramid's fused levels, then snapshot levels 0..4. The two are
    // independent, so they share every pipeline barrier below.
    if (pyramidFusedLevels_ != 0) {
        const DownsampleParams depthParams{extent_.width, extent_.height, pyramidFusedLevels_, 0};
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pyramidFusedPipeline_);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pyramidFusedPipelineLayout_, 0, 1,
                                &pyramidFusedSet_, 0, nullptr);
        vkCmdPushConstants(commandBuffer, pyramidFusedPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(depthParams), &depthParams);
        dispatchInput(commandBuffer, entry, groups(extent_.width, 16), groups(extent_.height, 16));
    }

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, snapshotPipeline_);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, snapshotPipelineLayout_, 0, 1,
                            &snapshotSet_, 0, nullptr);
    DownsampleParams snapshotParams{extent_.width, extent_.height, 0, snapshotMips_};
    vkCmdPushConstants(commandBuffer, snapshotPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(snapshotParams), &snapshotParams);
    dispatchInput(commandBuffer, entry, groups(extent_.width, 16), groups(extent_.height, 16));
    // Levels 5.. continue from level 4 (written above, read back here).
    bool snapshotPending = snapshotMips_ > SnapshotFusedLevels + 1;

    const auto levelWritten = [](const VkImage image, const std::uint32_t level) {
        return imageBarrier(image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, level);
    };
    const auto recordSnapshotTail = [&] {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, snapshotPipeline_);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, snapshotPipelineLayout_, 0, 1,
                                &snapshotSet_, 0, nullptr);
        snapshotParams.mode = 1;
        vkCmdPushConstants(commandBuffer, snapshotPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(snapshotParams), &snapshotParams);
        const std::uint32_t width = std::max(1U, extent_.width >> SnapshotFusedLevels);
        const std::uint32_t height = std::max(1U, extent_.height >> SnapshotFusedLevels);
        dispatchInput(commandBuffer, entry, groups(width, 16), groups(height, 16));
        snapshotPending = false;
    };
    for (std::uint32_t level = pyramidFusedLevels_ + 1; level <= pyramidLastLevel_; ++level) {
        std::array<VkImageMemoryBarrier2, 2> written{};
        std::uint32_t count = 0;
        // Level 1's source is the scene depth, ordered by `before`.
        if (level > 1)
            written[count++] = levelWritten(pyramid_.image(), level - 1);
        if (snapshotPending)
            written[count++] = levelWritten(snapshot_, SnapshotFusedLevels);
        if (count != 0)
            submitBarriers(commandBuffer, written.data(), count);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pyramidReducePipeline_);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pyramidPipelineLayout_, 0, 1,
                                &pyramidReduceSets_[level - pyramidFusedLevels_ - 1], 0, nullptr);
        const VkExtent2D extent = pyramid_.mipExtent(level);
        dispatchInput(commandBuffer, entry, groups(extent.width, 8), groups(extent.height, 8));
        if (snapshotPending)
            recordSnapshotTail();
    }
    if (snapshotPending) {
        const auto written = levelWritten(snapshot_, SnapshotFusedLevels);
        submitBarriers(commandBuffer, &written, 1);
        recordSnapshotTail();
    }

    const std::array after{
        imageBarrier(pyramid_.image(), VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, pyramid_.mipCount()),
        imageBarrier(snapshot_, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, snapshotMips_),
        // The colour target becomes the glass draw's attachment once the
        // snapshot has read it.
        imageBarrier(color, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
    };
    submitBarriers(commandBuffer, after);
    if (entry != inputGroups_.size())
        throw std::logic_error("GlassPass recorded fewer input dispatches than planned");
    pyramidInitialized_ = snapshotInitialized_ = true;
}


void GlassPass::record(const VkCommandBuffer commandBuffer, const RecordInfo& info) {
    if (!created() || info.color == VK_NULL_HANDLE || info.colorView == VK_NULL_HANDLE ||
        info.depth == VK_NULL_HANDLE || info.sceneDescriptorSet == VK_NULL_HANDLE)
        return;
    static const ProfileNameId inputsZone = Profiler::registerName("Glass.Inputs");
    static const ProfileNameId seedZone = Profiler::registerName("Glass.DepthSeed");
    static const ProfileNameId drawZone = Profiler::registerName("Glass.Draw");
    const auto beginZone = [&](const ProfileNameId name) {
        if (info.profiler != nullptr)
            info.profiler->beginZone(commandBuffer, info.profilerFrame, name);
    };
    const auto endZone = [&] {
        if (info.profiler != nullptr)
            info.profiler->endZone(commandBuffer, info.profilerFrame);
    };
    if (info.colorView != snapshotSourceView_) {
        // The set may still be referenced by a frame in flight; the view only
        // changes when its target is recreated, so this almost never waits.
        if (snapshotSourceView_ != VK_NULL_HANDLE && vkDeviceWaitIdle(device_) != VK_SUCCESS)
            throw std::runtime_error("Could not idle the device to rebind the glass snapshot source");
        const VkDescriptorImageInfo source{snapshotSampler_, info.colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         nullptr,
                                         snapshotSet_,
                                         0,
                                         0,
                                         1,
                                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                         &source,
                                         nullptr,
                                         nullptr};
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        snapshotSourceView_ = info.colorView;
    }
    // Without a visible pane every command below runs with zero size.
    recordGate(commandBuffer, info);
    // Snapshot and depth pyramid, recorded interleaved (one zone for both).
    beginZone(inputsZone);
    recordInputs(commandBuffer, info.color, info.depth);
    endZone();

    const VkImageAspectFlags glassDepthAspect =
        VK_IMAGE_ASPECT_DEPTH_BIT | (hasStencil(info_.depthFormat) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0U);
    const auto depthToAttachment = imageBarrier(
        glassDepth_, glassDepthAspect, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    submitBarriers(commandBuffer, &depthToAttachment, 1);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = info.colorView;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = glassDepthView_;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    // Every texel is written by the depth seed below.
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = extent_;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    rendering.pDepthAttachment = &depth;
    vkCmdBeginRendering(commandBuffer, &rendering);
    const VkViewport viewport{0.0F, 0.0F, static_cast<float>(extent_.width), static_cast<float>(extent_.height),
                              0.0F, 1.0F};
    const VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    constexpr VkDeviceSize vertexOffset{0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &info.vertexBuffer, &vertexOffset);
    vkCmdBindIndexBuffer(commandBuffer, info.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
    const std::array sets{info.sceneDescriptorSet, glassSet_};
    beginZone(seedZone);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, depthSeedPipeline_.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, depthSeedPipeline_.layout(), 0,
                            static_cast<std::uint32_t>(sets.size()), sets.data(), 0, nullptr);
    vkCmdDrawIndirect(commandBuffer, gate_.handle(), GateHeaderBytes, 1, sizeof(VkDrawIndirectCommand));
    endZone();
    const auto drawStream = [&](const GraphicsPipeline& pipeline, const Culling::IndexedIndirectDrawCount* stream,
                                const VkDeviceSize commandOffset) {
        if (stream == nullptr || !stream->valid())
            return;
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(), 0,
                                static_cast<std::uint32_t>(sets.size()), sets.data(), 0, nullptr);
        stream->record(commandBuffer, commandOffset, info.countOffset);
    };
    beginZone(drawZone);
    drawStream(singleSidedPipeline_, info.singleSided, info.commandOffset);
    drawStream(twoSidedPipeline_, info.twoSided, info.twoSidedCommandOffset);
    endZone();
    vkCmdEndRendering(commandBuffer);

    // Post-processing, Scene View display and the next frame sample it.
    const auto colorToSampled = imageBarrier(
        info.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, SampledReaderStages | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    submitBarriers(commandBuffer, &colorToSampled, 1);
}
} // namespace Engine
