#include "Engine/Renderer/Passes/GtaoPass.h"

#include "Engine/Renderer/Vulkan/PipelineCache.h"
#include "Engine/Renderer/shader_loader.h"

#include <algorithm>
#include <array>
#include <glm/glm.hpp>
#include <span>
#include <stdexcept>
#include <vector>

namespace Engine {
namespace {
// Command-buffer-local ranges work on graphics and async compute
// without mixing timestamp origins from different queues.
class GpuLabel final {
  public:
    GpuLabel(VkCommandBuffer commandBuffer, PFN_vkCmdBeginDebugUtilsLabelEXT begin, PFN_vkCmdEndDebugUtilsLabelEXT end,
             const char* name)
        : commandBuffer_(commandBuffer), end_(begin && end ? end : nullptr) {
        if (end_) {
            const VkDebugUtilsLabelEXT label{.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
                                             .pLabelName = name,
                                             .color = {0.2F, 0.7F, 0.9F, 1.0F}};
            begin(commandBuffer_, &label);
        }
    }
    ~GpuLabel() {
        if (end_)
            end_(commandBuffer_);
    }
    GpuLabel(const GpuLabel&) = delete;
    GpuLabel& operator=(const GpuLabel&) = delete;

  private:
    VkCommandBuffer commandBuffer_;
    PFN_vkCmdEndDebugUtilsLabelEXT end_;
};

struct DepthPrefilterSettings {
    glm::vec4 depthToView;
    float effectRadius, effectFalloffRange;
    std::uint32_t mipCount, writeBaseDepth;
};

struct MainSettings {
    glm::vec2 projectionScale;
    float radiusView, centerDepthScale;
    std::uint32_t frameIndex, useExternalNormals, maxSampleMip;
};

struct DenoiseSettings {
    float blurAmount;
};

struct UpsampleSettings {
    glm::vec2 inputToOutputScale;
    float depthSigma;
    std::uint32_t padding;
};

struct DenoiseUpsampleSettings {
    glm::vec2 inputToOutputScale;
    float depthSigma, blurAmount;
};

constexpr VkFormat RawAoFormat = VK_FORMAT_R16_SFLOAT, BaseDepthFormat = VK_FORMAT_R32_SFLOAT,
                   AuxiliaryFormat = VK_FORMAT_R8_UNORM;
constexpr float XeGtaoEffectFalloffRange = 0.615F;
constexpr float FinalBlurAmount = 1.2F, IntermediateBlurAmount = 1.2F / 5.F;
constexpr float UpsampleDepthSigma = 64.F;
// XeGTAO's centre-depth bias per depth precision: FP16 needs a far larger
// pull towards the camera than FP32 to stop flat surfaces self-occluding.
constexpr float CenterDepthScaleFp16 = 0.99920F, CenterDepthScaleFp32 = 0.99999F;

// The main-pass variants compiled by cmake/Shaders.cmake.
struct MainVariant {
    std::uint32_t directions, steps;
    bool halfResolution;
    const char* shader;
};
constexpr std::array<MainVariant, 6> MainVariants{{{1, 2, false, "shaders/gtao_main_low.spv"},
                                                   {2, 2, false, "shaders/gtao_main_medium.spv"},
                                                   {3, 3, false, "shaders/gtao_main_high.spv"},
                                                   {9, 3, false, "shaders/gtao_main_ultra.spv"},
                                                   {3, 2, true, "shaders/gtao_main_performance_half.spv"},
                                                   {4, 3, true, "shaders/gtao_main_balanced_half.spv"}}};

constexpr VkPipelineStageFlags2 ComputeStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr VkAccessFlags2 ShaderWrite = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
constexpr VkAccessFlags2 ShaderRead = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
constexpr VkDescriptorType CombinedSampler = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           SampledImage = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                           StorageImage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

std::uint16_t hilbertIndex(std::uint32_t x, std::uint32_t y) {
    x &= 63U;
    y &= 63U;
    std::uint32_t index = 0;
    for (std::uint32_t scale = 32; scale > 0; scale >>= 1U) {
        const std::uint32_t rx = (x & scale) != 0 ? 1U : 0U;
        const std::uint32_t ry = (y & scale) != 0 ? 1U : 0U;
        index += scale * scale * ((3U * rx) ^ ry);
        if (ry == 0) {
            if (rx != 0) {
                x = (scale * 2U - 1U) - x;
                y = (scale * 2U - 1U) - y;
            }
            std::swap(x, y);
        }
    }
    return static_cast<std::uint16_t>(index);
}

// Collects one synchronisation point so it costs a single vkCmdPipelineBarrier2.
// Images that stay in GENERAL are ordered by the global memory barrier; only
// layout transitions need per-image barriers.
class BarrierBatch final {
  public:
    void image(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, VkPipelineStageFlags2 srcStage,
               VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
               std::uint32_t levels = 1) {
        if (imageCount_ == images_.size())
            throw std::logic_error("Too many GTAO image barriers in one batch");
        VkImageMemoryBarrier2& b = images_[imageCount_++];
        b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = srcStage;
        b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage;
        b.dstAccessMask = dstAccess;
        b.oldLayout = oldLayout;
        b.newLayout = newLayout;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
    }
    void memory(VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                VkAccessFlags2 dstAccess) {
        memory_ = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        memory_.srcStageMask = srcStage;
        memory_.srcAccessMask = srcAccess;
        memory_.dstStageMask = dstStage;
        memory_.dstAccessMask = dstAccess;
        hasMemory_ = true;
    }
    void submit(VkCommandBuffer cmd) {
        if (imageCount_ == 0 && !hasMemory_)
            return;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = hasMemory_ ? 1 : 0;
        d.pMemoryBarriers = &memory_;
        d.imageMemoryBarrierCount = imageCount_;
        d.pImageMemoryBarriers = images_.data();
        vkCmdPipelineBarrier2(cmd, &d);
        imageCount_ = 0;
        hasMemory_ = false;
    }

  private:
    std::array<VkImageMemoryBarrier2, 6> images_{};
    std::uint32_t imageCount_ = 0;
    VkMemoryBarrier2 memory_{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    bool hasMemory_ = false;
};

VkPipeline makeCompute(VkDevice device, Assets::AssetManager& assets, const char* path, VkPipelineLayout layout) {
    auto shader = Vkutil::loadShaderModule(device, assets, path);
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader.get();
    stage.pName = "main";
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage = stage;
    info.layout = layout;
    VkPipeline pipeline{};
    if (vkCreateComputePipelines(device, PipelineCache::get(device), 1, &info, nullptr, &pipeline) != VK_SUCCESS)
        throw std::runtime_error("Could not create GTAO compute pipeline");
    return pipeline;
}
} // namespace
GtaoPass::~GtaoPass() {
    destroy();
}

void GtaoPass::create(VkPhysicalDevice physical, VkDevice device, VkCommandPool commandPool, VkQueue queue,
                      VkExtent2D fullExtent, VmaAllocator allocator, Assets::AssetManager& assets,
                      const GtaoQualitySettings quality, const std::span<const std::uint32_t> sharingFamilies) {
    if (!device || !fullExtent.width || !fullExtent.height)
        throw std::invalid_argument("GTAO requires a valid extent");
    if (quality.denoisePassCount > MaxDenoisePasses)
        throw std::invalid_argument("GTAO supports at most three denoise passes");
    // The main shader reads its centre depth from a fixed pyramid level, so
    // only native and exact half resolution are representable.
    if (quality.resolutionScale != 1.F && quality.resolutionScale != .5F)
        throw std::invalid_argument("GTAO supports only full or half resolution");
    const bool halfResolution = quality.resolutionScale != 1.F;
    const auto variant = std::ranges::find_if(MainVariants, [&](const MainVariant& candidate) {
        return candidate.directions == quality.directions && candidate.steps == quality.stepsPerDirection &&
               candidate.halfResolution == halfResolution;
    });
    if (variant == MainVariants.end())
        throw std::invalid_argument("No compiled GTAO main shader matches the requested quality");
    destroy();
    device_ = device;
    beginLabel_ = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
        vkGetDeviceProcAddr(device_, "vkCmdBeginDebugUtilsLabelEXT"));
    endLabel_ =
        reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(vkGetDeviceProcAddr(device_, "vkCmdEndDebugUtilsLabelEXT"));
    allocator_ = allocator;
    quality_ = quality;
    fullExtent_ = fullExtent;
    nativeResolution_ = !halfResolution;
    // Half resolution matches the pyramid's mip 1 exactly.
    halfExtent_ = halfResolution ? VkExtent2D{std::max(1u, fullExtent.width / 2), std::max(1u, fullExtent.height / 2)}
                                 : fullExtent;
    try {
        VkFormatProperties r8Properties{};
        vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R8_UNORM, &r8Properties);
        const VkFormatFeatureFlags aoRequiredFeatures =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
            (nativeResolution_ ? 0 : VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
        const VkFormat filteredAoFormat =
            (r8Properties.optimalTilingFeatures & aoRequiredFeatures) == aoRequiredFeatures ? VK_FORMAT_R8_UNORM
                                                                                            : VK_FORMAT_R16_SFLOAT;
        // Only the result and the debug views (raw_, filtered_) are sampled
        // outside GTAO's queue.  Everything else stays exclusive so drivers
        // may keep it compressed; it is discarded and fully rewritten every
        // frame, so no queue ownership transfer is needed either.
        raw_.create(physical, device_, halfExtent_, allocator, VK_FILTER_NEAREST, RawAoFormat, true, sharingFamilies);
        baseDepth_.create(physical, device_, halfExtent_, allocator, VK_FILTER_NEAREST, BaseDepthFormat, true);
        auxiliary_.create(physical, device_, halfExtent_, allocator, VK_FILTER_NEAREST, AuxiliaryFormat, true);
        filtered_.create(physical, device_, halfExtent_, allocator, VK_FILTER_NEAREST, filteredAoFormat, true,
                         sharingFamilies);
        if (quality_.denoisePassCount > 1)
            scratch_.create(physical, device_, halfExtent_, allocator, VK_FILTER_NEAREST, filteredAoFormat, true);
        if (!nativeResolution_)
            full_.create(physical, device_, fullExtent_, allocator, VK_FILTER_LINEAR, filteredAoFormat, true,
                         sharingFamilies);
        resultFormat_ = nativeResolution_ && quality_.denoisePassCount == 0 ? RawAoFormat : filteredAoFormat;
        std::array<std::uint32_t, 64 * 64> hilbertValues{};
        for (std::uint32_t y = 0; y < 64; ++y)
            for (std::uint32_t x = 0; x < 64; ++x)
                hilbertValues[y * 64 + x] = hilbertIndex(x, y);
        hilbertLut_.create(physical, device_, commandPool, queue, 64, 64,
                           std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(hilbertValues.data()),
                                                         hilbertValues.size() * sizeof(std::uint32_t)},
                           TextureColorSpace::Linear, false, allocator, TexturePixelFormat::R32_UINT);
        linearDepthMipCount_ = 1;
        for (auto d = std::max(fullExtent.width, fullExtent.height); d > 1; d >>= 1)
            ++linearDepthMipCount_;
        linearDepthMipCount_ = std::min(linearDepthMipCount_, std::max(1u, quality_.depthMipCount));
        if (halfResolution && linearDepthMipCount_ < 2)
            throw std::invalid_argument("Half-resolution GTAO needs at least two depth mips");
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D;
        VkFormatProperties fp16Properties{};
        vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R16_SFLOAT, &fp16Properties);
        const VkFormatFeatureFlags requiredFeatures =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        linearDepthFormat_ = (fp16Properties.optimalTilingFeatures & requiredFeatures) == requiredFeatures
                                 ? VK_FORMAT_R16_SFLOAT
                                 : VK_FORMAT_R32_SFLOAT;
        image.format = linearDepthFormat_;
        image.extent = {fullExtent.width, fullExtent.height, 1};
        image.mipLevels = linearDepthMipCount_;
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo alloc{};
        alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (vmaCreateImage(allocator, &image, &alloc, &linearDepthImage_, &linearDepthAllocation_, nullptr) !=
            VK_SUCCESS)
            throw std::runtime_error("Could not create GTAO depth pyramid");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = linearDepthImage_;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = linearDepthFormat_;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, linearDepthMipCount_, 0, 1};
        if (vkCreateImageView(device_, &view, nullptr, &linearDepthView_) != VK_SUCCESS)
            throw std::runtime_error("Could not create GTAO depth view");
        linearDepthMipViews_.resize(linearDepthMipCount_);
        for (uint32_t mip = 0; mip < linearDepthMipCount_; ++mip) {
            auto mipView = view;
            mipView.subresourceRange.baseMipLevel = mip;
            mipView.subresourceRange.levelCount = 1;
            if (vkCreateImageView(device_, &mipView, nullptr, &linearDepthMipViews_[mip]) != VK_SUCCESS)
                throw std::runtime_error("Could not create GTAO depth mip view");
        }
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod = float(linearDepthMipCount_ - 1);
        if (vkCreateSampler(device_, &sampler, nullptr, &linearDepthSampler_) != VK_SUCCESS)
            throw std::runtime_error("Could not create GTAO depth sampler");

        createKernel(Kernel::DepthPrefilter,
                     std::array{CombinedSampler, StorageImage, StorageImage, StorageImage, StorageImage, StorageImage,
                                StorageImage},
                     sizeof(DepthPrefilterSettings), assets, "shaders/gtao_prefilter_depth.spv");
        createKernel(Kernel::Main, std::array{CombinedSampler, CombinedSampler, SampledImage, StorageImage, StorageImage},
                     sizeof(MainSettings), assets, variant->shader);
        // At reduced resolution the last denoise pass is fused into the upsample.
        const std::uint32_t standaloneDenoisePasses =
            nativeResolution_ ? quality_.denoisePassCount : std::max(1u, quality_.denoisePassCount) - 1;
        if (standaloneDenoisePasses > 0)
            createKernel(Kernel::Denoise, std::array{CombinedSampler, CombinedSampler, StorageImage},
                         sizeof(DenoiseSettings), assets, "shaders/gtao_denoise.spv");
        if (!nativeResolution_ && quality_.denoisePassCount == 0)
            createKernel(Kernel::Upsample, std::array{CombinedSampler, CombinedSampler, CombinedSampler, StorageImage},
                         sizeof(UpsampleSettings), assets, "shaders/gtao_upsample_compute.spv");
        if (!nativeResolution_ && quality_.denoisePassCount > 0)
            createKernel(Kernel::DenoiseUpsample,
                         std::array{CombinedSampler, CombinedSampler, CombinedSampler, CombinedSampler, StorageImage,
                                    StorageImage},
                         sizeof(DenoiseUpsampleSettings), assets, "shaders/gtao_denoise_upsample.spv");

        const auto setsPerFrame = [&](std::size_t k) {
            if (!kernels_[k].pipeline)
                return 0u;
            return static_cast<Kernel>(k) == Kernel::Denoise ? standaloneDenoisePasses : 1u;
        };
        std::array<VkDescriptorPoolSize, 3> poolSizes{
            {{CombinedSampler, 0}, {SampledImage, 0}, {StorageImage, 0}}};
        std::uint32_t setCount = 0;
        for (std::size_t k = 0; k < KernelCount; ++k) {
            const std::uint32_t sets = setsPerFrame(k) * FramesInFlight;
            setCount += sets;
            for (std::uint32_t b = 0; b < kernels_[k].bindingCount; ++b)
                for (auto& size : poolSizes)
                    if (size.type == kernels_[k].bindingTypes[b])
                        size.descriptorCount += sets;
        }
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = setCount;
        poolInfo.poolSizeCount = uint32_t(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS)
            throw std::runtime_error("Could not create GTAO descriptor pool");
        for (std::size_t k = 0; k < KernelCount; ++k)
            for (std::uint32_t frame = 0; frame < FramesInFlight; ++frame)
                for (std::uint32_t pass = 0; pass < setsPerFrame(k); ++pass) {
                    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                    ai.descriptorPool = descriptorPool_;
                    ai.descriptorSetCount = 1;
                    ai.pSetLayouts = &kernels_[k].setLayout;
                    if (vkAllocateDescriptorSets(device_, &ai, &slots_[k][frame][pass].set) != VK_SUCCESS)
                        throw std::runtime_error("Could not allocate GTAO descriptor sets");
                }
    } catch (...) {
        destroy();
        throw;
    }
}

void GtaoPass::createKernel(const Kernel id, const std::span<const VkDescriptorType> bindings,
                            const std::uint32_t pushConstantSize, Assets::AssetManager& assets, const char* shader) {
    ComputeKernel& k = kernel(id);
    std::array<VkDescriptorSetLayoutBinding, MaxBindings> layoutBindings{};
    for (std::uint32_t b = 0; b < bindings.size(); ++b) {
        layoutBindings[b] = {b, bindings[b], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        k.bindingTypes[b] = bindings[b];
    }
    k.bindingCount = static_cast<std::uint32_t>(bindings.size());
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = k.bindingCount;
    layoutInfo.pBindings = layoutBindings.data();
    if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &k.setLayout) != VK_SUCCESS)
        throw std::runtime_error("Could not create GTAO descriptor set layout");
    const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushConstantSize};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &k.setLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &k.pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("Could not create GTAO pipeline layout");
    k.pipeline = makeCompute(device_, assets, shader, k.pipelineLayout);
}

void GtaoPass::bindKernel(VkCommandBuffer cmd, const Kernel id, DescriptorSlot& slot,
                          const std::span<const VkDescriptorImageInfo> images, const void* pushConstants,
                          const std::uint32_t pushConstantSize) {
    const ComputeKernel& k = kernel(id);
    if (images.size() != k.bindingCount)
        throw std::logic_error("GTAO descriptor count does not match its kernel layout");
    const auto same = [](const VkDescriptorImageInfo& a, const VkDescriptorImageInfo& b) {
        return a.sampler == b.sampler && a.imageView == b.imageView && a.imageLayout == b.imageLayout;
    };
    if (!slot.valid || !std::equal(images.begin(), images.end(), slot.written.begin(), same)) {
        std::array<VkWriteDescriptorSet, MaxBindings> writes{};
        for (std::uint32_t b = 0; b < k.bindingCount; ++b)
            writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                         nullptr,
                         slot.set,
                         b,
                         0,
                         1,
                         k.bindingTypes[b],
                         &images[b],
                         nullptr,
                         nullptr};
        vkUpdateDescriptorSets(device_, k.bindingCount, writes.data(), 0, nullptr);
        std::copy(images.begin(), images.end(), slot.written.begin());
        slot.valid = true;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k.pipelineLayout, 0, 1, &slot.set, 0, nullptr);
    vkCmdPushConstants(cmd, k.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pushConstantSize, pushConstants);
}

void GtaoPass::clearImages(VkCommandBuffer cmd, const bool graphOwnsResultState) {
    // Internal images are discarded at the start of every record(); only
    // the images Forward or a debug view may sample before GTAO first runs
    // need white contents.
    const VkImage result = resultImage();
    const VkImage graphResult = graphOwnsResultState ? result : VK_NULL_HANDLE;
    const VkPipelineStageFlags2 readStages =
        graphOwnsResultState ? ComputeStage : ComputeStage | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    const VkPipelineStageFlags2 sampledStage =
        graphOwnsResultState ? ComputeStage : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    std::array<VkImage, 3> images{};
    std::uint32_t imageCount = 0;
    for (const VkImage image : {raw_.image(), filtered_.image(), full_.image()})
        if (image && image != graphResult)
            images[imageCount++] = image;
    BarrierBatch barriers;
    for (std::uint32_t i = 0; i < imageCount; ++i)
        barriers.image(images[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    barriers.submit(cmd);
    const VkClearColorValue white{{1, 1, 1, 1}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (std::uint32_t i = 0; i < imageCount; ++i)
        vkCmdClearColorImage(cmd, images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);
    for (std::uint32_t i = 0; i < imageCount; ++i) {
        // The result rests in SHADER_READ_ONLY between frames; the debug
        // views read the internal AO images in GENERAL.
        if (images[i] == result)
            barriers.image(images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, sampledStage,
                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        else
            barriers.image(images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                           VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, readStages,
                           ShaderWrite | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    barriers.submit(cmd);
    initialized_ = true;
}

void GtaoPass::initialize(VkCommandBuffer cmd) {
    if (!initialized_)
        clearImages(cmd);
}

void GtaoPass::record(VkCommandBuffer cmd, uint32_t frame, uint32_t sampleIndex, VkImageView depth,
                      VkSampler depthSampler, VkImageView viewNormal, VkSampler viewNormalSampler,
                      bool useExternalNormals, const Mat4& inverseProjection, const GtaoExternalState externalState) {
    if (frame >= FramesInFlight)
        throw std::out_of_range("Invalid GTAO frame slot");
    const GpuLabel gtaoLabel{cmd, beginLabel_, endLabel_, "GTAO"};
    const bool transitionResult = !externalState.graphOwnsResultState;
    if (!initialized_)
        clearImages(cmd, externalState.graphOwnsResultState);
    const VkImage result = resultImage();
    BarrierBatch barriers;
    // Internal images are fully rewritten every frame, so their previous
    // contents are discarded.  That also lets them change queues freely.
    for (const VkImage image : {baseDepth_.image(), auxiliary_.image(), scratch_.image()})
        if (image)
            barriers.image(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, ComputeStage, 0, ComputeStage,
                           ShaderWrite);
    barriers.image(linearDepthImage_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, ComputeStage, 0,
                   ComputeStage, ShaderWrite, linearDepthMipCount_);
    if (transitionResult)
        barriers.image(result, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, ComputeStage,
                       ShaderWrite);
    // raw_ and filtered_ stay in GENERAL for the debug views; order this
    // frame's writes after the previous frame's GTAO reads and writes.
    barriers.memory(ComputeStage, ShaderWrite, ComputeStage, ShaderWrite);
    barriers.submit(cmd);
    const auto afterWrites = [&] {
        barriers.memory(ComputeStage, ShaderWrite, ComputeStage, ShaderRead | ShaderWrite);
        barriers.submit(cmd);
    };

    const glm::mat4 inverse = inverseProjection.native();
    {
        const GpuLabel depthLabel{cmd, beginLabel_, endLabel_, "GTAO.DepthPrefilter"};
        std::array<VkDescriptorImageInfo, 7> images{};
        images[0] = {depthSampler, depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        for (uint32_t binding = 1; binding < 6; ++binding)
            images[binding] = {VK_NULL_HANDLE, linearDepthMipViews_[std::min(binding - 1, linearDepthMipCount_ - 1)],
                               VK_IMAGE_LAYOUT_GENERAL};
        images[6] = {VK_NULL_HANDLE, baseDepth_.imageView(), VK_IMAGE_LAYOUT_GENERAL};
        // Only view.z and view.w of inverseProjection * (0, 0, z, 1) matter.
        const DepthPrefilterSettings settings{{inverse[2][2], inverse[3][2], inverse[2][3], inverse[3][3]},
                                              1.F,
                                              XeGtaoEffectFalloffRange,
                                              linearDepthMipCount_,
                                              nativeResolution_ ? 0U : 1U};
        bindKernel(cmd, Kernel::DepthPrefilter, slot(Kernel::DepthPrefilter, frame), images, &settings,
                   sizeof(settings));
        vkCmdDispatch(cmd, (fullExtent_.width + 15) / 16, (fullExtent_.height + 15) / 16, 1);
    }
    afterWrites();
    {
        const GpuLabel mainLabel{cmd, beginLabel_, endLabel_, "GTAO.Main"};
        const std::array<VkDescriptorImageInfo, 5> images{{
            {linearDepthSampler_, linearDepthView_, VK_IMAGE_LAYOUT_GENERAL},
            {viewNormalSampler, viewNormal,
             useExternalNormals ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
            {VK_NULL_HANDLE, hilbertLut_.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {VK_NULL_HANDLE, raw_.imageView(), VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, auxiliary_.imageView(), VK_IMAGE_LAYOUT_GENERAL},
        }};
        const uint32_t sourceMip = nativeResolution_ ? 0U : 1U;
        const MainSettings settings{{1.f / inverse[0][0], 1.f / inverse[1][1]},
                                    1.f,
                                    linearDepthFormat_ == VK_FORMAT_R16_SFLOAT ? CenterDepthScaleFp16
                                                                               : CenterDepthScaleFp32,
                                    sampleIndex,
                                    useExternalNormals ? 1U : 0U,
                                    linearDepthMipCount_ - 1 - sourceMip};
        bindKernel(cmd, Kernel::Main, slot(Kernel::Main, frame), images, &settings, sizeof(settings));
        vkCmdDispatch(cmd, (halfExtent_.width + 7) / 8, (halfExtent_.height + 7) / 8, 1);
    }
    afterWrites();
    const HdrBuffer* denoised = &raw_;
    const std::uint32_t standaloneDenoisePasses =
        nativeResolution_ ? quality_.denoisePassCount : std::max(1u, quality_.denoisePassCount) - 1;
    for (uint32_t pass = 0; pass < standaloneDenoisePasses; ++pass) {
        const GpuLabel denoiseLabel{cmd, beginLabel_, endLabel_, "GTAO.Denoise"};
        // Ping-pong so the final pass (standalone or fused) lands in filtered_.
        HdrBuffer& output = (quality_.denoisePassCount - pass) % 2 == 1 ? filtered_ : scratch_;
        const std::array<VkDescriptorImageInfo, 3> images{{
            {denoised->sampler(), denoised->imageView(), VK_IMAGE_LAYOUT_GENERAL},
            {auxiliary_.sampler(), auxiliary_.imageView(), VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, output.imageView(), VK_IMAGE_LAYOUT_GENERAL},
        }};
        const DenoiseSettings settings{pass + 1 == quality_.denoisePassCount ? FinalBlurAmount
                                                                             : IntermediateBlurAmount};
        bindKernel(cmd, Kernel::Denoise, slot(Kernel::Denoise, frame, pass), images, &settings, sizeof(settings));
        vkCmdDispatch(cmd, (halfExtent_.width + 15) / 16, (halfExtent_.height + 7) / 8, 1);
        afterWrites();
        denoised = &output;
    }
    if (!nativeResolution_) {
        const GpuLabel upsampleLabel{cmd, beginLabel_, endLabel_, "GTAO.Upsample"};
        const glm::vec2 scale{float(halfExtent_.width) / float(fullExtent_.width),
                              float(halfExtent_.height) / float(fullExtent_.height)};
        const VkDescriptorImageInfo linearDepth{linearDepthSampler_, linearDepthView_, VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo baseDepth{baseDepth_.sampler(), baseDepth_.imageView(), VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo fullAo{VK_NULL_HANDLE, full_.imageView(), VK_IMAGE_LAYOUT_GENERAL};
        if (quality_.denoisePassCount == 0) {
            const std::array<VkDescriptorImageInfo, 4> images{
                {{denoised->sampler(), denoised->imageView(), VK_IMAGE_LAYOUT_GENERAL}, linearDepth, baseDepth, fullAo}};
            const UpsampleSettings settings{scale, UpsampleDepthSigma, 0};
            bindKernel(cmd, Kernel::Upsample, slot(Kernel::Upsample, frame), images, &settings, sizeof(settings));
        } else {
            const std::array<VkDescriptorImageInfo, 6> images{{
                {denoised->sampler(), denoised->imageView(), VK_IMAGE_LAYOUT_GENERAL},
                {auxiliary_.sampler(), auxiliary_.imageView(), VK_IMAGE_LAYOUT_GENERAL},
                linearDepth,
                baseDepth,
                fullAo,
                {VK_NULL_HANDLE, filtered_.imageView(), VK_IMAGE_LAYOUT_GENERAL},
            }};
            const DenoiseUpsampleSettings settings{scale, UpsampleDepthSigma, FinalBlurAmount};
            bindKernel(cmd, Kernel::DenoiseUpsample, slot(Kernel::DenoiseUpsample, frame), images, &settings,
                       sizeof(settings));
        }
        vkCmdDispatch(cmd, (fullExtent_.width + 7) / 8, (fullExtent_.height + 7) / 8, 1);
    }
    if (transitionResult) {
        barriers.image(result, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, ComputeStage,
                       ShaderWrite, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        barriers.submit(cmd);
    }
}

void GtaoPass::reset() noexcept {
    initialized_ = false;
}

void GtaoPass::destroy() noexcept {
    if (device_) {
        if (descriptorPool_)
            vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        for (const ComputeKernel& k : kernels_) {
            if (k.pipeline)
                vkDestroyPipeline(device_, k.pipeline, nullptr);
            if (k.pipelineLayout)
                vkDestroyPipelineLayout(device_, k.pipelineLayout, nullptr);
            if (k.setLayout)
                vkDestroyDescriptorSetLayout(device_, k.setLayout, nullptr);
        }
        if (linearDepthSampler_)
            vkDestroySampler(device_, linearDepthSampler_, nullptr);
        for (auto v : linearDepthMipViews_)
            if (v)
                vkDestroyImageView(device_, v, nullptr);
        if (linearDepthView_)
            vkDestroyImageView(device_, linearDepthView_, nullptr);
        if (linearDepthImage_)
            vmaDestroyImage(allocator_, linearDepthImage_, linearDepthAllocation_);
    }
    raw_.destroy();
    baseDepth_.destroy();
    auxiliary_.destroy();
    filtered_.destroy();
    scratch_.destroy();
    full_.destroy();
    hilbertLut_.destroy();
    kernels_ = {};
    slots_ = {};
    descriptorPool_ = {};
    linearDepthMipViews_.clear();
    linearDepthImage_ = {};
    linearDepthFormat_ = VK_FORMAT_UNDEFINED;
    linearDepthAllocation_ = {};
    linearDepthView_ = {};
    linearDepthSampler_ = {};
    linearDepthMipCount_ = 0;
    nativeResolution_ = false;
    resultFormat_ = VK_FORMAT_UNDEFINED;
    allocator_ = {};
    device_ = {};
    beginLabel_ = nullptr;
    endLabel_ = nullptr;
    reset();
}
} // namespace Engine
