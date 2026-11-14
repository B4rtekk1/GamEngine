#include "Engine/Renderer/Passes/GlassPass.h"

#include "Engine/Assets/AssetManager.h"
#include "Engine/Renderer/Culling/IndexedIndirectDrawCount.h"
#include "Engine/Renderer/Geometry/GpuVertex.h"
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
        createDescriptors();
        createGraphicsPipelines();
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
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(info_.allocator, &imageInfo, &allocation, &snapshot_, &snapshotMemory_, nullptr) !=
        VK_SUCCESS)
        throw std::runtime_error("Could not allocate the glass scene snapshot");
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = snapshot_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = HdrBuffer::Format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, snapshotMips_, 0, 1};
    if (vkCreateImageView(device_, &viewInfo, nullptr, &snapshotView_) != VK_SUCCESS)
        throw std::runtime_error("Could not create the glass scene snapshot view");
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

void GlassPass::createPyramidPipelines() {
    const std::array bindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
                                     nullptr},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &pyramidSetLayout_) != VK_SUCCESS)
        throw std::runtime_error("Could not create the glass depth pyramid set layout");
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &pyramidSetLayout_;
    if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pyramidPipelineLayout_) != VK_SUCCESS)
        throw std::runtime_error("Could not create the glass depth pyramid pipeline layout");
    const auto makeCompute = [&](const char* shader) {
        const auto module = Vkutil::loadShaderModule(device_, *info_.assets, shader);
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = module.get();
        pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = pyramidPipelineLayout_;
        VkPipeline pipeline{VK_NULL_HANDLE};
        if (vkCreateComputePipelines(device_, PipelineCache::get(device_), 1, &pipelineInfo, nullptr, &pipeline) !=
            VK_SUCCESS)
            throw std::runtime_error(std::string("Could not create glass compute pipeline: ") + shader);
        return pipeline;
    };
    // Mip 0 is a plain copy of the hardware depth, the same as culling's.
    pyramidInitPipeline_ = makeCompute("shaders/hiz_initialize.spv");
    pyramidReducePipeline_ = makeCompute("shaders/glass_depth_reduce.spv");
}

void GlassPass::createDescriptors() {
    const std::uint32_t mips = pyramid_.mipCount();
    const std::array sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, mips + 2U},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, mips},
    };
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = mips + 1U;
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    poolInfo.pPoolSizes = sizes.data();
    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS)
        throw std::runtime_error("Could not create the glass descriptor pool");
    pyramidPass_.create(device_, descriptorPool_, pyramidInitPipeline_, pyramidPipelineLayout_, pyramidSetLayout_,
                        pyramidReducePipeline_, pyramidPipelineLayout_, pyramidSetLayout_, pyramid_,
                        info_.sceneDepthView, info_.sceneDepthSampler);

    const std::array bindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                     nullptr},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                     nullptr},
    };
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &glassSetLayout_) != VK_SUCCESS)
        throw std::runtime_error("Could not create the glass set layout");
    VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocateInfo.descriptorPool = descriptorPool_;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &glassSetLayout_;
    if (vkAllocateDescriptorSets(device_, &allocateInfo, &glassSet_) != VK_SUCCESS)
        throw std::runtime_error("Could not allocate the glass descriptor set");
    const VkDescriptorImageInfo snapshotInfo{snapshotSampler_, snapshotView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo pyramidInfo{pyramid_.sampler(), pyramid_.fullView(),
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const std::array writes{
        VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, glassSet_, 0, 0, 1,
                             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &snapshotInfo, nullptr, nullptr},
        VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, glassSet_, 1, 0, 1,
                             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &pyramidInfo, nullptr, nullptr},
    };
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
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
    pyramidPass_.destroy();
    if (device_ != VK_NULL_HANDLE) {
        if (descriptorPool_ != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        if (glassSetLayout_ != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device_, glassSetLayout_, nullptr);
        for (const VkPipeline pipeline : {pyramidInitPipeline_, pyramidReducePipeline_})
            if (pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device_, pipeline, nullptr);
        if (pyramidPipelineLayout_ != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(device_, pyramidPipelineLayout_, nullptr);
        if (pyramidSetLayout_ != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device_, pyramidSetLayout_, nullptr);
        if (snapshotSampler_ != VK_NULL_HANDLE)
            vkDestroySampler(device_, snapshotSampler_, nullptr);
        if (snapshotView_ != VK_NULL_HANDLE)
            vkDestroyImageView(device_, snapshotView_, nullptr);
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
    descriptorPool_ = VK_NULL_HANDLE;
    glassSetLayout_ = VK_NULL_HANDLE;
    glassSet_ = VK_NULL_HANDLE;
    pyramidInitPipeline_ = pyramidReducePipeline_ = VK_NULL_HANDLE;
    pyramidPipelineLayout_ = VK_NULL_HANDLE;
    pyramidSetLayout_ = VK_NULL_HANDLE;
    snapshotSampler_ = VK_NULL_HANDLE;
    snapshotView_ = VK_NULL_HANDLE;
    glassDepthView_ = VK_NULL_HANDLE;
    snapshot_ = glassDepth_ = VK_NULL_HANDLE;
    snapshotMemory_ = glassDepthMemory_ = VK_NULL_HANDLE;
    snapshotMips_ = 0;
    snapshotInitialized_ = pyramidInitialized_ = false;
    device_ = VK_NULL_HANDLE;
    extent_ = {};
    info_ = {};
}

void GlassPass::recordSnapshot(const VkCommandBuffer commandBuffer, const VkImage color) {
    // The previous frame sampled the snapshot; its contents are replaced.
    const std::array before{
        imageBarrier(color, VK_IMAGE_ASPECT_COLOR_BIT, ColorProducerStages, ColorProducerAccess,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
        imageBarrier(snapshot_, VK_IMAGE_ASPECT_COLOR_BIT,
                     snapshotInitialized_ ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
                     snapshotInitialized_ ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : VK_ACCESS_2_NONE,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, snapshotMips_),
    };
    submitBarriers(commandBuffer, before);
    const VkImageCopy copy{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           {0, 0, 0},
                           {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           {0, 0, 0},
                           {extent_.width, extent_.height, 1}};
    vkCmdCopyImage(commandBuffer, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, snapshot_,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    // Box-filtered mip chain. The shader samples it trilinearly with a small
    // rotated tap pattern, which hides the 2x2 box structure.
    for (std::uint32_t mip = 1; mip < snapshotMips_; ++mip) {
        const auto toSource = imageBarrier(snapshot_, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                           VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                           VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mip - 1);
        submitBarriers(commandBuffer, &toSource, 1);
        const auto size = [&](const std::uint32_t level) {
            return VkOffset3D{static_cast<std::int32_t>(std::max(1U, extent_.width >> level)),
                              static_cast<std::int32_t>(std::max(1U, extent_.height >> level)), 1};
        };
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, 1};
        blit.srcOffsets[1] = size(mip - 1);
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
        blit.dstOffsets[1] = size(mip);
        vkCmdBlitImage(commandBuffer, snapshot_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, snapshot_,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }
    std::array<VkImageMemoryBarrier2, 3> after{};
    std::uint32_t afterCount = 0;
    if (snapshotMips_ > 1)
        after[afterCount++] = imageBarrier(snapshot_, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                           VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, snapshotMips_ - 1);
    after[afterCount++] = imageBarrier(snapshot_, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                       VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, snapshotMips_ - 1);
    // The colour target becomes the glass draw's attachment.
    after[afterCount++] = imageBarrier(
        color, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    submitBarriers(commandBuffer, after.data(), afterCount);
    snapshotInitialized_ = true;
}

void GlassPass::recordPyramid(const VkCommandBuffer commandBuffer, const VkImage sceneDepth) {
    const VkImageAspectFlags depthAspect =
        VK_IMAGE_ASPECT_DEPTH_BIT | (hasStencil(info_.depthFormat) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0U);
    const std::array before{
        // Depth stays read-only; this only orders its last attachment write
        // before the compute copy.
        imageBarrier(sceneDepth, depthAspect,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL),
        imageBarrier(pyramid_.image(), VK_IMAGE_ASPECT_COLOR_BIT,
                     pyramidInitialized_ ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
                     pyramidInitialized_ ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : VK_ACCESS_2_NONE,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, pyramid_.mipCount()),
    };
    submitBarriers(commandBuffer, before);
    pyramidPass_.record(commandBuffer, pyramid_, VK_NULL_HANDLE, false);
    const auto toSampled = imageBarrier(pyramid_.image(), VK_IMAGE_ASPECT_COLOR_BIT,
                                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
                                        pyramid_.mipCount());
    submitBarriers(commandBuffer, &toSampled, 1);
    pyramidInitialized_ = true;
}

void GlassPass::record(const VkCommandBuffer commandBuffer, const RecordInfo& info) {
    if (!created() || info.color == VK_NULL_HANDLE || info.colorView == VK_NULL_HANDLE ||
        info.depth == VK_NULL_HANDLE || info.sceneDescriptorSet == VK_NULL_HANDLE)
        return;
    recordPyramid(commandBuffer, info.depth);
    recordSnapshot(commandBuffer, info.color);

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
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, depthSeedPipeline_.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, depthSeedPipeline_.layout(), 0,
                            static_cast<std::uint32_t>(sets.size()), sets.data(), 0, nullptr);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    const auto drawStream = [&](const GraphicsPipeline& pipeline, const Culling::IndexedIndirectDrawCount* stream,
                                const VkDeviceSize commandOffset) {
        if (stream == nullptr || !stream->valid())
            return;
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(), 0,
                                static_cast<std::uint32_t>(sets.size()), sets.data(), 0, nullptr);
        stream->record(commandBuffer, commandOffset, info.countOffset);
    };
    drawStream(singleSidedPipeline_, info.singleSided, info.commandOffset);
    drawStream(twoSidedPipeline_, info.twoSided, info.twoSidedCommandOffset);
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
