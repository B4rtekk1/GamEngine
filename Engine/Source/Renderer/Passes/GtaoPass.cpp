#include "Engine/Renderer/Passes/GtaoPass.h"

#include "Engine/Renderer/shader_loader.h"

#include <algorithm>
#include <array>
#include <span>
#include <glm/glm.hpp>
#include <stdexcept>
#include <vector>

namespace Engine {
    namespace {
        struct DepthPrefilterSettings {
            glm::mat4 inverseProjection;
            float effectRadius, effectFalloffRange;
            std::uint32_t mipCount;
        };

        struct MainSettings {
            glm::vec2 projectionScale;
            float radiusView, falloff;
            std::uint32_t frameIndex;
            float padding;
            std::uint32_t maxSampleMip;

            MainSettings(glm::vec2 projection, float radius, float falloffValue, std::uint32_t frame, float pad,
                         std::uint32_t maxMip)
                : projectionScale(projection), radiusView(radius), falloff(falloffValue), frameIndex(frame),
                  padding(pad), maxSampleMip(maxMip) {
            }
        };

        struct DenoiseSettings {
            float blurAmount;
        };

        struct UpsampleSettings {
            glm::vec2 inputToOutputScale;
            float depthSigma;
            std::uint32_t sourceMip;
        };

        constexpr VkFormat RawAoFormat = VK_FORMAT_R16_SFLOAT, BaseDepthFormat = VK_FORMAT_R32_SFLOAT,
                AuxiliaryFormat = VK_FORMAT_R8_UNORM;
        constexpr float XeGtaoEffectFalloffRange = 0.615F;

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

        void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                     VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                     VkAccessFlags2 dstAccess, uint32_t baseMip = 0, uint32_t levels = 1) {
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = srcStage;
            b.srcAccessMask = srcAccess;
            b.dstStageMask = dstStage;
            b.dstAccessMask = dstAccess;
            b.oldLayout = oldLayout;
            b.newLayout = newLayout;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseMip, levels, 0, 1};
            VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            d.imageMemoryBarrierCount = 1;
            d.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &d);
        }

        VkPipeline makeCompute(VkDevice device, Assets::AssetManager &assets, const char *path,
                               VkPipelineLayout layout) {
            auto shader = Vkutil::loadShaderModule(device, assets, path);
            VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            stage.module = shader.get();
            stage.pName = "main";
            VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            info.stage = stage;
            info.layout = layout;
            VkPipeline pipeline{};
            if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
                throw std::runtime_error("Could not create GTAO compute pipeline");
            return pipeline;
        }
    } // namespace
    GtaoPass::~GtaoPass() {
        destroy();
    }

    void GtaoPass::create(VkPhysicalDevice physical, VkDevice device, VkCommandPool commandPool, VkQueue queue,
                          VkExtent2D fullExtent, VmaAllocator allocator,
                          Assets::AssetManager &assets, const GtaoQualitySettings quality,
                          const std::span<const std::uint32_t> sharingFamilies) {
        if (!device || !fullExtent.width || !fullExtent.height)
            throw std::invalid_argument("GTAO requires a valid extent");
        if (quality.denoisePassCount > 3)
            throw std::invalid_argument("GTAO supports at most three denoise passes");
        destroy();
        device_ = device;
        allocator_ = allocator;
        quality_ = quality;
        fullExtent_ = fullExtent;
        halfExtent_ = {
            std::max(1u, uint32_t(float(fullExtent.width) * quality_.resolutionScale)),
            std::max(1u, uint32_t(float(fullExtent.height) * quality_.resolutionScale))
        };
        nativeResolution_ = halfExtent_.width == fullExtent_.width && halfExtent_.height == fullExtent_.height;
        try {
            VkFormatProperties r8Properties{};
            vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R8_UNORM, &r8Properties);
            const VkFormatFeatureFlags aoRequiredFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                                             VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                    (nativeResolution_ ? 0 : VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
            const VkFormat filteredAoFormat =
                    (r8Properties.optimalTilingFeatures & aoRequiredFeatures) == aoRequiredFeatures
                        ? VK_FORMAT_R8_UNORM
                        : VK_FORMAT_R16_SFLOAT;
            raw_.create(physical, device_, halfExtent_, allocator, VK_FILTER_NEAREST, RawAoFormat, true, sharingFamilies);
            baseDepth_.create(physical, device_, halfExtent_, allocator, VK_FILTER_NEAREST, BaseDepthFormat, true, sharingFamilies);
            auxiliary_.create(physical, device_, halfExtent_, allocator, VK_FILTER_NEAREST, AuxiliaryFormat, true, sharingFamilies);
            filtered_.create(physical, device_, halfExtent_, allocator, VK_FILTER_NEAREST, filteredAoFormat, true, sharingFamilies);
            if (quality_.denoisePassCount > 1)
                scratch_.create(physical, device_, halfExtent_, allocator, VK_FILTER_NEAREST, filteredAoFormat, true, sharingFamilies);
            if (!nativeResolution_)
                full_.create(physical, device_, fullExtent_, allocator, VK_FILTER_LINEAR, filteredAoFormat, true, sharingFamilies);
            resultFormat_ = nativeResolution_ && quality_.denoisePassCount == 0
                ? RawAoFormat : filteredAoFormat;
            std::array<std::uint16_t, 64 * 64> hilbertValues{};
            for (std::uint32_t y = 0; y < 64; ++y)
                for (std::uint32_t x = 0; x < 64; ++x)
                    hilbertValues[y * 64 + x] = hilbertIndex(x, y);
            hilbertLut_.create(
                physical, device_, commandPool, queue, 64, 64,
                std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t *>(hilbertValues.data()),
                                              hilbertValues.size() * sizeof(std::uint16_t)},
                TextureColorSpace::Linear, false, allocator, TexturePixelFormat::R16_UINT);
            linearDepthMipCount_ = 1;
            for (auto d = std::max(fullExtent.width, fullExtent.height); d > 1; d >>= 1)
                ++linearDepthMipCount_;
            linearDepthMipCount_ = std::min(linearDepthMipCount_, std::max(1u, quality_.depthMipCount));
            VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            image.imageType = VK_IMAGE_TYPE_2D;
            VkFormatProperties fp16Properties{};
            vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R16_SFLOAT, &fp16Properties);
            const VkFormatFeatureFlags requiredFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                                            VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
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
            image.sharingMode = sharingFamilies.size() > 1 ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
            image.queueFamilyIndexCount = sharingFamilies.size() > 1
                ? static_cast<std::uint32_t>(sharingFamilies.size()) : 0;
            image.pQueueFamilyIndices = sharingFamilies.size() > 1 ? sharingFamilies.data() : nullptr;
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
            std::array<VkDescriptorSetLayoutBinding, 6> depthBindings{};
            depthBindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
            for (uint32_t binding = 1; binding < depthBindings.size(); ++binding)
                depthBindings[binding] = {binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
            VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            dl.bindingCount = uint32_t(depthBindings.size());
            dl.pBindings = depthBindings.data();
            if (vkCreateDescriptorSetLayout(device_, &dl, nullptr, &depthLayout_) != VK_SUCCESS)
                throw std::runtime_error("Could not create GTAO depth layout");
            VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            pi.setLayoutCount = 1;
            pi.pSetLayouts = &depthLayout_;
            VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DepthPrefilterSettings)};
            pi.pushConstantRangeCount = 1;
            pi.pPushConstantRanges = &pc;
            if (vkCreatePipelineLayout(device_, &pi, nullptr, &depthPipelineLayout_) != VK_SUCCESS)
                throw std::runtime_error("Could not create GTAO depth pipeline layout");
            depthPipeline_ = makeCompute(device_, assets, "shaders/gtao_prefilter_depth.spv", depthPipelineLayout_);
            std::array<VkDescriptorPoolSize, 2> depthSizes{
                {
                    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, FramesInFlight},
                    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, FramesInFlight * 5}
                }
            };
            VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            dp.maxSets = FramesInFlight;
            dp.poolSizeCount = 2;
            dp.pPoolSizes = depthSizes.data();
            if (vkCreateDescriptorPool(device_, &dp, nullptr, &depthDescriptorPool_) != VK_SUCCESS)
                throw std::runtime_error("Could not create GTAO depth pool");
            std::array<VkDescriptorSetLayout, FramesInFlight> ls{};
            ls.fill(depthLayout_);
            VkDescriptorSetAllocateInfo lai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            lai.descriptorPool = depthDescriptorPool_;
            lai.descriptorSetCount = FramesInFlight;
            lai.pSetLayouts = ls.data();
            if (vkAllocateDescriptorSets(device_, &lai, depthSets_.data()) != VK_SUCCESS)
                throw std::runtime_error("Could not allocate GTAO depth sets");
            const std::array<uint32_t, 3> inputCount{4, 2, 3}, outputCount{3, 1, 1},
                    pushSize{sizeof(MainSettings), sizeof(DenoiseSettings), sizeof(UpsampleSettings)};
            const std::array<const char *, 3> shader{
                "", "shaders/gtao_denoise.spv",
                "shaders/gtao_upsample_compute.spv"
            };
            for (uint32_t p = 0; p < 3; ++p) {
                std::vector<VkDescriptorSetLayoutBinding> b;
                for (uint32_t i = 0; i < inputCount[p]; ++i)
                    b.push_back({
                        i, p == 0 && i == 3 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                            : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                        1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr
                    });
                for (uint32_t i = 0; i < outputCount[p]; ++i)
                    b.push_back(
                        {inputCount[p] + i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
                VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
                ci.bindingCount = uint32_t(b.size());
                ci.pBindings = b.data();
                if (vkCreateDescriptorSetLayout(device_, &ci, nullptr, &computeLayouts_[p]) != VK_SUCCESS)
                    throw std::runtime_error("Could not create GTAO compute layout");
                VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushSize[p]};
                VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
                pli.setLayoutCount = 1;
                pli.pSetLayouts = &computeLayouts_[p];
                pli.pushConstantRangeCount = 1;
                pli.pPushConstantRanges = &pc;
                if (vkCreatePipelineLayout(device_, &pli, nullptr, &computePipelineLayouts_[p]) != VK_SUCCESS)
                    throw std::runtime_error("Could not create GTAO compute pipeline layout");
                if (p != 0)
                    computePipelines_[p] = makeCompute(device_, assets, shader[p], computePipelineLayouts_[p]);
            }
            const std::array<const char *, 6> mainShaders{
                "shaders/gtao_main_low.spv", "shaders/gtao_main_medium.spv",
                "shaders/gtao_main_high.spv", "shaders/gtao_main_ultra.spv",
                "shaders/gtao_main_performance_half.spv", "shaders/gtao_main_balanced_half.spv"
            };
            for (uint32_t q = 0; q < mainShaders.size(); ++q)
                mainQualityPipelines_[q] = makeCompute(device_, assets, mainShaders[q], computePipelineLayouts_[0]);
            std::array<VkDescriptorPoolSize, 3> cs{
                {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 31},
                 {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, FramesInFlight},
                 {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 19}}
            };
            VkDescriptorPoolCreateInfo cp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            cp.maxSets = 10;
            cp.poolSizeCount = 3;
            cp.pPoolSizes = cs.data();
            if (vkCreateDescriptorPool(device_, &cp, nullptr, &computeDescriptorPool_) != VK_SUCCESS)
                throw std::runtime_error("Could not create GTAO compute pool");
            for (uint32_t p = 0; p < 3; ++p) {
                std::array<VkDescriptorSetLayout, FramesInFlight> l{};
                l.fill(computeLayouts_[p]);
                VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                ai.descriptorPool = computeDescriptorPool_;
                ai.descriptorSetCount = FramesInFlight;
                ai.pSetLayouts = l.data();
                if (vkAllocateDescriptorSets(device_, &ai, computeSets_[p].data()) != VK_SUCCESS)
                    throw std::runtime_error("Could not allocate GTAO compute sets");
            }
            for (auto &sets : extraDenoiseSets_) {
                std::array<VkDescriptorSetLayout, FramesInFlight> layouts{};
                layouts.fill(computeLayouts_[1]);
                VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                ai.descriptorPool = computeDescriptorPool_;
                ai.descriptorSetCount = FramesInFlight;
                ai.pSetLayouts = layouts.data();
                if (vkAllocateDescriptorSets(device_, &ai, sets.data()) != VK_SUCCESS)
                    throw std::runtime_error("Could not allocate GTAO denoise sets");
            }
        } catch (...) {
            destroy();
            throw;
        }
    }

    void GtaoPass::clearImages(VkCommandBuffer cmd, const bool graphOwnsResultState) {
        const VkImage graphResult = graphOwnsResultState ? resultImage() : VK_NULL_HANDLE;
        const VkPipelineStageFlags2 readStages = graphOwnsResultState
            ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
            : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        const VkPipelineStageFlags2 sampledStage = graphOwnsResultState
            ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        std::vector<VkImage> images;
        const auto addInternalImage = [&](const VkImage image) {
            if (image != graphResult) images.push_back(image);
        };
        for (const VkImage image : {raw_.image(), baseDepth_.image(), auxiliary_.image(), filtered_.image()})
            addInternalImage(image);
        if (quality_.denoisePassCount > 1)
            addInternalImage(scratch_.image());
        if (!nativeResolution_)
            addInternalImage(full_.image());
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        for (auto image: images)
            barrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_NONE,
                    0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkClearColorValue white{{1, 1, 1, 1}};
        for (auto image: images)
            vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);
        for (auto image: {raw_.image(), baseDepth_.image(), auxiliary_.image()})
            if (image != graphResult)
                barrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        readStages,
                        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        if (nativeResolution_ && quality_.denoisePassCount == 0 && raw_.image() != graphResult)
            barrier(cmd, raw_.image(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    sampledStage, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        if (quality_.denoisePassCount > 1)
            barrier(cmd, scratch_.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        if (nativeResolution_) {
            if (filtered_.image() != graphResult)
                barrier(cmd, filtered_.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT, sampledStage,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
        else {
            barrier(cmd, filtered_.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    readStages,
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            if (full_.image() != graphResult)
                barrier(cmd, full_.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT, sampledStage,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
        initialized_ = true;
    }

    void GtaoPass::initialize(VkCommandBuffer cmd) {
        if (!initialized_)
            clearImages(cmd);
    }

    void GtaoPass::buildLinearDepth(VkCommandBuffer cmd, uint32_t frame, VkImageView depth, VkSampler depthSampler,
                                    const Mat4 &inverseProjection) {
        barrier(cmd, linearDepthImage_,
                linearDepthInitialized_ ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL,
                linearDepthInitialized_ ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
                linearDepthInitialized_ ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                0, linearDepthMipCount_);
        VkDescriptorImageInfo src{depthSampler, depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        std::array<VkDescriptorImageInfo, 6> images{};
        images[0] = src;
        for (uint32_t binding = 1; binding < images.size(); ++binding)
            images[binding] = {VK_NULL_HANDLE,
                               linearDepthMipViews_[std::min(binding - 1, linearDepthMipCount_ - 1)],
                               VK_IMAGE_LAYOUT_GENERAL};
        std::array<VkWriteDescriptorSet, 6> writes{};
        for (uint32_t binding = 0; binding < writes.size(); ++binding) {
            writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, depthSets_[frame], binding, 0, 1,
                               binding == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                               &images[binding], nullptr, nullptr};
        }
        vkUpdateDescriptorSets(device_, uint32_t(writes.size()), writes.data(), 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, depthPipeline_);
        auto set = depthSets_[frame];
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, depthPipelineLayout_, 0, 1, &set, 0, nullptr);
        const DepthPrefilterSettings pc{
            inverseProjection.native(), 1.F, XeGtaoEffectFalloffRange, linearDepthMipCount_
        };
        vkCmdPushConstants(cmd, depthPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, (fullExtent_.width + 15) / 16, (fullExtent_.height + 15) / 16, 1);
        barrier(cmd, linearDepthImage_, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 0, linearDepthMipCount_);
        linearDepthInitialized_ = true;
    }

    void GtaoPass::record(VkCommandBuffer cmd, uint32_t frame, uint32_t sampleIndex, VkImageView depth,
                          VkSampler depthSampler, VkImageView viewNormal, VkSampler viewNormalSampler,
                          bool useExternalNormals, const Mat4 &inverseProjection,
                          const GtaoExternalState externalState) {
        if (frame >= FramesInFlight)
            throw std::out_of_range("Invalid GTAO frame slot");
        const bool transitionResult = !externalState.graphOwnsResultState;
        if (!initialized_)
            clearImages(cmd, externalState.graphOwnsResultState);
        buildLinearDepth(cmd, frame, depth, depthSampler, inverseProjection);
        const auto beginInternalWrite = [&](VkImage image, VkPipelineStageFlags2 previousStages) {
            barrier(cmd, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, previousStages,
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        };
        if (nativeResolution_ && quality_.denoisePassCount == 0) {
            if (transitionResult)
                barrier(cmd, raw_.image(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        }
        else
            beginInternalWrite(raw_.image(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        beginInternalWrite(baseDepth_.image(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        beginInternalWrite(auxiliary_.image(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        if (nativeResolution_ && quality_.denoisePassCount != 0) {
            if (transitionResult)
                barrier(cmd, filtered_.image(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        }
        else if (!nativeResolution_) {
            beginInternalWrite(filtered_.image(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
            if (transitionResult)
                barrier(cmd, full_.image(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        }
        if (quality_.denoisePassCount > 1)
            beginInternalWrite(scratch_.image(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        auto update = [&](uint32_t p, std::initializer_list<VkDescriptorImageInfo> in,
                          std::initializer_list<VkImageView> out, uint32_t denoiseIndex = 0) {
            const VkDescriptorSet descriptorSet = p == 1 && denoiseIndex > 0
                ? extraDenoiseSets_[denoiseIndex - 1][frame] : computeSets_[p][frame];
            std::vector<VkDescriptorImageInfo> infos(in);
            for (auto image: out)
                infos.push_back({VK_NULL_HANDLE, image, VK_IMAGE_LAYOUT_GENERAL});
            std::vector<VkWriteDescriptorSet> w;
            for (uint32_t b = 0; b < infos.size(); ++b)
                w.push_back({
                    VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, b, 0, 1,
                    b < in.size()
                        ? (p == 0 && b == 3 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                            : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
                        : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    &infos[b], nullptr, nullptr
                });
            const auto same = [](const VkDescriptorImageInfo &a, const VkDescriptorImageInfo &b) {
                return a.sampler == b.sampler && a.imageView == b.imageView && a.imageLayout == b.imageLayout;
            };
            auto &cached = p == 1 && denoiseIndex > 0
                ? extraDenoiseDescriptorCache_[denoiseIndex - 1][frame] : computeDescriptorCache_[p][frame];
            const bool changed = cached.size() != infos.size() ||
                                 !std::equal(cached.begin(), cached.end(), infos.begin(), same);
            if (changed) {
                vkUpdateDescriptorSets(device_, uint32_t(w.size()), w.data(), 0, nullptr);
                cached = std::move(infos);
            }
        };
        auto dispatch = [&](uint32_t p, auto &constants, VkExtent2D extent, uint32_t denoiseIndex = 0) {
            auto set = p == 1 && denoiseIndex > 0
                ? extraDenoiseSets_[denoiseIndex - 1][frame] : computeSets_[p][frame];
            VkPipeline pipeline = computePipelines_[p];
            if (p == 0) {
                const uint32_t qualityIndex = quality_.directions == 1 && quality_.stepsPerDirection == 2 ? 0U
                                            : quality_.directions == 2 && quality_.stepsPerDirection == 2 ? 1U
                                            : quality_.directions == 3 && quality_.stepsPerDirection == 3 ? 2U
                                            : quality_.directions == 9 && quality_.stepsPerDirection == 3 ? 3U
                                            : quality_.directions == 3 && quality_.stepsPerDirection == 2 ? 4U
                                                                                                          : 5U;
                pipeline = mainQualityPipelines_[qualityIndex];
            }
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computePipelineLayouts_[p], 0, 1, &set, 0,
                                    nullptr);
            vkCmdPushConstants(cmd, computePipelineLayouts_[p], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                               &constants);
            vkCmdDispatch(cmd, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
        };
        auto inverse = inverseProjection.native();
        const uint32_t sourceMip = quality_.resolutionScale == 1.F ? 0U : 1U;
        update(0, {
                   {linearDepthSampler_, linearDepthView_, VK_IMAGE_LAYOUT_GENERAL},
                   {viewNormalSampler, viewNormal, useExternalNormals
                       ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                       : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
                   {linearDepthSampler_, linearDepthView_, VK_IMAGE_LAYOUT_GENERAL},
                   {VK_NULL_HANDLE, hilbertLut_.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
               },
               {raw_.imageView(), auxiliary_.imageView(), baseDepth_.imageView()});
        MainSettings main{{1.f / inverse[0][0], 1.f / inverse[1][1]}, 1.f, 1.f, sampleIndex, 0.f,
                          linearDepthMipCount_ - 1 - sourceMip};
        main.padding = useExternalNormals ? 1.f : 0.f;
        dispatch(0, main, halfExtent_);
        barrier(cmd, raw_.image(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        barrier(cmd, auxiliary_.image(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        barrier(cmd, baseDepth_.image(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        const HdrBuffer *denoised = &raw_;
        for (uint32_t pass = 0; pass < quality_.denoisePassCount; ++pass) {
            HdrBuffer &output = (quality_.denoisePassCount - pass) % 2 == 1 ? filtered_ : scratch_;
            if (pass > 1)
                barrier(cmd, output.image(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            update(1,
                   {{denoised->sampler(), denoised->imageView(), VK_IMAGE_LAYOUT_GENERAL},
                    {auxiliary_.sampler(), auxiliary_.imageView(), VK_IMAGE_LAYOUT_GENERAL}},
                   {output.imageView()}, pass);
            DenoiseSettings denoise{pass + 1 == quality_.denoisePassCount ? 1.2F : 1.2F / 5.F};
            dispatch(1, denoise, halfExtent_, pass);
            barrier(cmd, output.image(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            denoised = &output;
        }
        if (!nativeResolution_) {
            update(2,
                   {
                       {denoised->sampler(), denoised->imageView(), VK_IMAGE_LAYOUT_GENERAL},
                       {linearDepthSampler_, linearDepthView_, VK_IMAGE_LAYOUT_GENERAL},
                       {baseDepth_.sampler(), baseDepth_.imageView(), VK_IMAGE_LAYOUT_GENERAL}
                   },
                   {full_.imageView()});
            UpsampleSettings up{
                {
                    float(halfExtent_.width) / float(fullExtent_.width),
                    float(halfExtent_.height) / float(fullExtent_.height)
                },
                64.F,
                0
            };
            dispatch(2, up, fullExtent_);
        }
        const VkImage finalImage = nativeResolution_ ? denoised->image() : full_.image();
        if (transitionResult)
            barrier(cmd, finalImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }

    void GtaoPass::reset() noexcept {
        initialized_ = false;
    }

    void GtaoPass::destroy() noexcept {
        for (auto &passCaches: computeDescriptorCache_)
            for (auto &cache: passCaches) cache.clear();
        for (auto &passCaches: extraDenoiseDescriptorCache_)
            for (auto &cache: passCaches) cache.clear();
        if (device_) {
            if (computeDescriptorPool_)
                vkDestroyDescriptorPool(device_, computeDescriptorPool_, nullptr);
            for (auto p: computePipelines_)
                if (p)
                    vkDestroyPipeline(device_, p, nullptr);
            for (auto p: mainQualityPipelines_)
                if (p)
                    vkDestroyPipeline(device_, p, nullptr);
            for (auto p: computePipelineLayouts_)
                if (p)
                    vkDestroyPipelineLayout(device_, p, nullptr);
            for (auto p: computeLayouts_)
                if (p)
                    vkDestroyDescriptorSetLayout(device_, p, nullptr);
            if (depthDescriptorPool_)
                vkDestroyDescriptorPool(device_, depthDescriptorPool_, nullptr);
            if (depthPipeline_)
                vkDestroyPipeline(device_, depthPipeline_, nullptr);
            if (depthPipelineLayout_)
                vkDestroyPipelineLayout(device_, depthPipelineLayout_, nullptr);
            if (depthLayout_)
                vkDestroyDescriptorSetLayout(device_, depthLayout_, nullptr);
            if (linearDepthSampler_)
                vkDestroySampler(device_, linearDepthSampler_, nullptr);
            for (auto v: linearDepthMipViews_)
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
        computeLayouts_.fill({});
        computePipelineLayouts_.fill({});
        computePipelines_.fill({});
        mainQualityPipelines_.fill({});
        for (auto &s: computeSets_)
            s.fill({});
        for (auto &s: extraDenoiseSets_)
            s.fill({});
        computeDescriptorPool_ = {};
        depthLayout_ = {};
        depthPipelineLayout_ = {};
        depthPipeline_ = {};
        depthDescriptorPool_ = {};
        depthSets_.fill({});
        linearDepthMipViews_.clear();
        linearDepthImage_ = {};
        linearDepthFormat_ = VK_FORMAT_UNDEFINED;
        linearDepthAllocation_ = {};
        linearDepthView_ = {};
        linearDepthSampler_ = {};
        linearDepthMipCount_ = 0;
        linearDepthInitialized_ = false;
        nativeResolution_ = false;
        resultFormat_ = VK_FORMAT_UNDEFINED;
        allocator_ = {};
        device_ = {};
        reset();
    }
} // namespace Engine
