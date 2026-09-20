#include "Engine/Renderer/Passes/GtaoPass.h"

#include "Engine/Renderer/shader_loader.h"

#include <algorithm>
#include <array>
#include <glm/glm.hpp>
#include <stdexcept>
#include <vector>

namespace Engine {
    namespace {
        struct LinearizeSettings {
            glm::mat4 inverseProjection;
        };

        struct DepthMipSettings {
            float effectRadius, effectFalloffRange;
        };

        struct MainSettings {
            glm::vec2 projectionScale;
            float radiusView, falloff;
            std::uint32_t frameIndex;
            float padding;
            std::uint32_t directions, stepsPerDirection, sourceMip, maxSampleMip;
            inline static std::uint32_t profileDirections = 8, profileSteps = 4;

            MainSettings(glm::vec2 projection, float radius, float falloffValue, std::uint32_t frame, float pad)
                : projectionScale(projection), radiusView(radius), falloff(falloffValue), frameIndex(frame),
                  padding(pad),
                  directions(profileDirections), stepsPerDirection(profileSteps), sourceMip(0), maxSampleMip(0) {
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

        constexpr VkFormat AoFormat = VK_FORMAT_R16_SFLOAT, LinearDepthFormat = VK_FORMAT_R32_SFLOAT,
                AuxiliaryFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

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

    void GtaoPass::create(VkPhysicalDevice physical, VkDevice device, VkExtent2D fullExtent, VmaAllocator allocator,
                          Assets::AssetManager &assets, const GtaoQualitySettings quality) {
        if (!device || !fullExtent.width || !fullExtent.height)
            throw std::invalid_argument("GTAO requires a valid extent");
        destroy();
        device_ = device;
        allocator_ = allocator;
        quality_ = quality;
        MainSettings::profileDirections = quality_.directions;
        MainSettings::profileSteps = quality_.stepsPerDirection;
        fullExtent_ = fullExtent;
        halfExtent_ = {
            std::max(1u, uint32_t(float(fullExtent.width) * quality_.resolutionScale)),
            std::max(1u, uint32_t(float(fullExtent.height) * quality_.resolutionScale))
        };
        nativeResolution_ = halfExtent_.width == fullExtent_.width && halfExtent_.height == fullExtent_.height;
        try {
            raw_.create(physical, device_, halfExtent_, allocator, VK_FILTER_NEAREST, AoFormat, true);
            baseDepth_.create(physical, device_, halfExtent_, allocator, VK_FILTER_NEAREST, LinearDepthFormat, true);
            auxiliary_.create(physical, device_, halfExtent_, allocator, VK_FILTER_NEAREST, AuxiliaryFormat, true);
            filtered_.create(physical, device_, halfExtent_, allocator, VK_FILTER_NEAREST, AoFormat, true);
            if (!nativeResolution_)
                full_.create(physical, device_, fullExtent_, allocator, VK_FILTER_LINEAR, AoFormat, true);
            linearDepthMipCount_ = 1;
            for (auto d = std::max(fullExtent.width, fullExtent.height); d > 1; d >>= 1)
                ++linearDepthMipCount_;
            linearDepthMipCount_ = std::min(linearDepthMipCount_, std::max(1u, quality_.depthMipCount));
            VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            image.imageType = VK_IMAGE_TYPE_2D;
            image.format = LinearDepthFormat;
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
            view.format = LinearDepthFormat;
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
            const std::array<VkDescriptorSetLayoutBinding, 2> depthBindings{
                {
                    {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                    {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}
                }
            };
            VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            dl.bindingCount = 2;
            dl.pBindings = depthBindings.data();
            for (auto &l: depthLayouts_)
                if (vkCreateDescriptorSetLayout(device_, &dl, nullptr, &l) != VK_SUCCESS)
                    throw std::runtime_error("Could not create GTAO depth layout");
            for (uint32_t i = 0; i < 2; ++i) {
                VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
                pi.setLayoutCount = 1;
                pi.pSetLayouts = &depthLayouts_[i];
                VkPushConstantRange pc{
                    VK_SHADER_STAGE_COMPUTE_BIT, 0,
                    i == 0 ? sizeof(LinearizeSettings) : sizeof(DepthMipSettings)
                };
                pi.pushConstantRangeCount = 1;
                pi.pPushConstantRanges = &pc;
                if (vkCreatePipelineLayout(device_, &pi, nullptr, &depthPipelineLayouts_[i]) != VK_SUCCESS)
                    throw std::runtime_error("Could not create GTAO depth pipeline layout");
            }
            depthPipelines_[0] = makeCompute(device_, assets, "shaders/gtao_linearize_depth.spv",
                                             depthPipelineLayouts_[0]);
            depthPipelines_[1] =
                    makeCompute(device_, assets, "shaders/gtao_depth_downsample.spv", depthPipelineLayouts_[1]);
            std::array<VkDescriptorPoolSize, 2> depthSizes{
                {
                    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, FramesInFlight + linearDepthMipCount_ - 1},
                    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, FramesInFlight + linearDepthMipCount_ - 1}
                }
            };
            VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            dp.maxSets = FramesInFlight + linearDepthMipCount_ - 1;
            dp.poolSizeCount = 2;
            dp.pPoolSizes = depthSizes.data();
            if (vkCreateDescriptorPool(device_, &dp, nullptr, &depthDescriptorPool_) != VK_SUCCESS)
                throw std::runtime_error("Could not create GTAO depth pool");
            std::array<VkDescriptorSetLayout, FramesInFlight> ls{};
            ls.fill(depthLayouts_[0]);
            VkDescriptorSetAllocateInfo lai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            lai.descriptorPool = depthDescriptorPool_;
            lai.descriptorSetCount = FramesInFlight;
            lai.pSetLayouts = ls.data();
            if (vkAllocateDescriptorSets(device_, &lai, linearizeSets_.data()) != VK_SUCCESS)
                throw std::runtime_error("Could not allocate GTAO depth sets");
            depthReduceSets_.resize(linearDepthMipCount_ - 1);
            std::vector<VkDescriptorSetLayout> rls(linearDepthMipCount_ - 1, depthLayouts_[1]);
            VkDescriptorSetAllocateInfo rai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            rai.descriptorPool = depthDescriptorPool_;
            rai.descriptorSetCount = uint32_t(rls.size());
            rai.pSetLayouts = rls.data();
            if (vkAllocateDescriptorSets(device_, &rai, depthReduceSets_.data()) != VK_SUCCESS)
                throw std::runtime_error("Could not allocate GTAO mip sets");
            for (uint32_t mip = 1; mip < linearDepthMipCount_; ++mip) {
                VkDescriptorImageInfo src{linearDepthSampler_, linearDepthMipViews_[mip - 1], VK_IMAGE_LAYOUT_GENERAL},
                        dst{VK_NULL_HANDLE, linearDepthMipViews_[mip], VK_IMAGE_LAYOUT_GENERAL};
                std::array<VkWriteDescriptorSet, 2> w{
                    {
                        {
                            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, depthReduceSets_[mip - 1], 0, 0, 1,
                            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &src, nullptr, nullptr
                        },
                        {
                            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, depthReduceSets_[mip - 1], 1, 0, 1,
                            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &dst, nullptr, nullptr
                        }
                    }
                };
                vkUpdateDescriptorSets(device_, 2, w.data(), 0, nullptr);
            }
            const std::array<uint32_t, 3> inputCount{3, 2, 3}, outputCount{3, 1, 1},
                    pushSize{sizeof(MainSettings), sizeof(DenoiseSettings), sizeof(UpsampleSettings)};
            const std::array<const char *, 3> shader{
                "shaders/gtao_main.spv", "shaders/gtao_denoise.spv",
                "shaders/gtao_upsample_compute.spv"
            };
            for (uint32_t p = 0; p < 3; ++p) {
                std::vector<VkDescriptorSetLayoutBinding> b;
                for (uint32_t i = 0; i < inputCount[p]; ++i)
                    b.push_back({
                        i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr
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
                computePipelines_[p] = makeCompute(device_, assets, shader[p], computePipelineLayouts_[p]);
            }
            std::array<VkDescriptorPoolSize, 2> cs{
                {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 24}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 15}}
            };
            VkDescriptorPoolCreateInfo cp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            cp.maxSets = 9;
            cp.poolSizeCount = 2;
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
        } catch (...) {
            destroy();
            throw;
        }
    }

    void GtaoPass::clearImages(VkCommandBuffer cmd) {
        std::vector<VkImage> images{raw_.image(), baseDepth_.image(), auxiliary_.image(), filtered_.image()};
        if (!nativeResolution_)
            images.push_back(full_.image());
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        for (auto image: images)
            barrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_NONE,
                    0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkClearColorValue white{{1, 1, 1, 1}};
        for (auto image: images)
            vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);
        for (auto image: images)
            barrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        initialized_ = true;
    }

    void GtaoPass::initialize(VkCommandBuffer cmd) {
        if (!initialized_)
            clearImages(cmd);
    }

    void GtaoPass::buildLinearDepth(VkCommandBuffer cmd, uint32_t frame, VkImageView depth, VkSampler depthSampler,
                                    const Mat4 &inverseProjection) {
        barrier(cmd, linearDepthImage_,
                linearDepthInitialized_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, 0, linearDepthMipCount_);
        VkDescriptorImageInfo src{depthSampler, depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
                dst{VK_NULL_HANDLE, linearDepthMipViews_[0], VK_IMAGE_LAYOUT_GENERAL};
        const auto &cachedSource = linearizeSources_[frame];
        if (!linearizeDescriptorsValid_[frame] || cachedSource.sampler != src.sampler ||
            cachedSource.imageView != src.imageView || cachedSource.imageLayout != src.imageLayout) {
            std::array<VkWriteDescriptorSet, 2> w{
                {
                    {
                        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, linearizeSets_[frame], 0,
                        0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &src, nullptr, nullptr
                    },
                    {
                        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, linearizeSets_[frame], 1,
                        0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &dst, nullptr, nullptr
                    }
                }
            };
            vkUpdateDescriptorSets(device_, 2, w.data(), 0, nullptr);
            linearizeSources_[frame] = src;
            linearizeDescriptorsValid_[frame] = true;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, depthPipelines_[0]);
        auto set = linearizeSets_[frame];
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, depthPipelineLayouts_[0], 0, 1, &set, 0, nullptr);
        LinearizeSettings pc{inverseProjection.native()};
        vkCmdPushConstants(cmd, depthPipelineLayouts_[0], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, (fullExtent_.width + 7) / 8, (fullExtent_.height + 7) / 8, 1);
        const DepthMipSettings mipSettings{1.F, 1.F};
        for (uint32_t mip = 1; mip < linearDepthMipCount_; ++mip) {
            barrier(cmd, linearDepthImage_, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, mip - 1, 1);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, depthPipelines_[1]);
            set = depthReduceSets_[mip - 1];
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, depthPipelineLayouts_[1], 0, 1, &set, 0,
                                    nullptr);
            vkCmdPushConstants(cmd, depthPipelineLayouts_[1], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mipSettings),
                               &mipSettings);
            vkCmdDispatch(cmd, (std::max(1u, fullExtent_.width >> mip) + 7) / 8,
                          (std::max(1u, fullExtent_.height >> mip) + 7) / 8, 1);
        }
        barrier(cmd, linearDepthImage_, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 0, linearDepthMipCount_);
        linearDepthInitialized_ = true;
    }

    void GtaoPass::record(VkCommandBuffer cmd, uint32_t frame, uint32_t sampleIndex, VkImageView depth,
                          VkSampler depthSampler, VkImageView viewNormal, VkSampler viewNormalSampler,
                          bool useExternalNormals, const Mat4 &inverseProjection) {
        if (frame >= FramesInFlight)
            throw std::out_of_range("Invalid GTAO frame slot");
        if (!initialized_)
            clearImages(cmd);
        buildLinearDepth(cmd, frame, depth, depthSampler, inverseProjection);
        std::vector<VkImage> work{raw_.image(), baseDepth_.image(), auxiliary_.image(), filtered_.image()};
        if (!nativeResolution_)
            work.push_back(full_.image());
        for (auto image: work)
            barrier(cmd, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        auto update = [&](uint32_t p, std::initializer_list<VkDescriptorImageInfo> in,
                          std::initializer_list<VkImageView> out) {
            std::vector<VkDescriptorImageInfo> infos(in);
            for (auto image: out)
                infos.push_back({VK_NULL_HANDLE, image, VK_IMAGE_LAYOUT_GENERAL});
            std::vector<VkWriteDescriptorSet> w;
            for (uint32_t b = 0; b < infos.size(); ++b)
                w.push_back({
                    VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, computeSets_[p][frame], b, 0, 1,
                    b < in.size() ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    &infos[b], nullptr, nullptr
                });
            const auto same = [](const VkDescriptorImageInfo &a, const VkDescriptorImageInfo &b) {
                return a.sampler == b.sampler && a.imageView == b.imageView && a.imageLayout == b.imageLayout;
            };
            auto &cached = computeDescriptorCache_[p][frame];
            const bool changed = cached.size() != infos.size() ||
                                 !std::equal(cached.begin(), cached.end(), infos.begin(), same);
            if (changed) {
                vkUpdateDescriptorSets(device_, uint32_t(w.size()), w.data(), 0, nullptr);
                cached = std::move(infos);
            }
        };
        auto dispatch = [&](uint32_t p, auto &constants, VkExtent2D extent) {
            auto set = computeSets_[p][frame];
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computePipelines_[p]);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computePipelineLayouts_[p], 0, 1, &set, 0,
                                    nullptr);
            vkCmdPushConstants(cmd, computePipelineLayouts_[p], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                               &constants);
            vkCmdDispatch(cmd, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
        };
        auto inverse = inverseProjection.native();
        const uint32_t sourceMip = std::min(quality_.resolutionScale == 1.F ? 0U : 1U, linearDepthMipCount_ - 1);
        update(0, {
                   {linearDepthSampler_, linearDepthView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                   {viewNormalSampler, viewNormal, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                   {linearDepthSampler_, linearDepthView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
               },
               {raw_.imageView(), auxiliary_.imageView(), baseDepth_.imageView()});
        MainSettings main{{1.f / inverse[0][0], 1.f / inverse[1][1]}, 1.f, 1.f, sampleIndex, 0.f};
        main.padding = useExternalNormals ? 1.f : 0.f;
        main.sourceMip = sourceMip;
        main.maxSampleMip = linearDepthMipCount_ - 1 - sourceMip;
        dispatch(0, main, halfExtent_);
        barrier(cmd, raw_.image(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        barrier(cmd, auxiliary_.image(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        barrier(cmd, baseDepth_.image(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        update(1,
               {
                   {raw_.sampler(), raw_.imageView(), VK_IMAGE_LAYOUT_GENERAL},
                   {auxiliary_.sampler(), auxiliary_.imageView(), VK_IMAGE_LAYOUT_GENERAL}
               },
               {filtered_.imageView()});
        DenoiseSettings denoise{1.F};
        dispatch(1, denoise, halfExtent_);
        barrier(cmd, filtered_.image(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        if (!nativeResolution_) {
            update(2,
                   {
                       {filtered_.sampler(), filtered_.imageView(), VK_IMAGE_LAYOUT_GENERAL},
                       {linearDepthSampler_, linearDepthView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
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
        for (auto image: work)
            barrier(cmd, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }

    void GtaoPass::reset() noexcept {
        initialized_ = false;
    }

    void GtaoPass::destroy() noexcept {
        linearizeSources_.fill({});
        linearizeDescriptorsValid_.fill(false);
        for (auto &passCaches: computeDescriptorCache_)
            for (auto &cache: passCaches) cache.clear();
        if (device_) {
            if (computeDescriptorPool_)
                vkDestroyDescriptorPool(device_, computeDescriptorPool_, nullptr);
            for (auto p: computePipelines_)
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
            for (auto p: depthPipelines_)
                if (p)
                    vkDestroyPipeline(device_, p, nullptr);
            for (auto p: depthPipelineLayouts_)
                if (p)
                    vkDestroyPipelineLayout(device_, p, nullptr);
            for (auto p: depthLayouts_)
                if (p)
                    vkDestroyDescriptorSetLayout(device_, p, nullptr);
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
        full_.destroy();
        computeLayouts_.fill({});
        computePipelineLayouts_.fill({});
        computePipelines_.fill({});
        for (auto &s: computeSets_)
            s.fill({});
        computeDescriptorPool_ = {};
        depthLayouts_.fill({});
        depthPipelineLayouts_.fill({});
        depthPipelines_.fill({});
        depthDescriptorPool_ = {};
        linearizeSets_.fill({});
        depthReduceSets_.clear();
        linearDepthMipViews_.clear();
        linearDepthImage_ = {};
        linearDepthAllocation_ = {};
        linearDepthView_ = {};
        linearDepthSampler_ = {};
        linearDepthMipCount_ = 0;
        linearDepthInitialized_ = false;
        nativeResolution_ = false;
        allocator_ = {};
        device_ = {};
        reset();
    }
} // namespace Engine
