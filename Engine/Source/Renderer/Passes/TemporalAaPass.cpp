#include "Engine/Renderer/Passes/TemporalAaPass.h"
#include "Engine/Renderer/shader_loader.h"

#include <stdexcept>

namespace Engine {
    namespace {
        struct Settings {
            float currentJitterX, currentJitterY, previousJitterX, previousJitterY;
            float historyWeight, inverseWidth, inverseHeight, padding;
            float projectionA, projectionB;
        };
    }

    TemporalAaPass::~TemporalAaPass() { destroy(); }

    void TemporalAaPass::create(const VkPhysicalDevice physicalDevice, const VkDevice device,
                                const VkExtent2D extent, const VmaAllocator allocator,
                                const VkImageView currentView, const VkSampler sampler,
                                const VkImageView velocityView, const VkSampler velocitySampler,
                                const VkImageView currentDepthView, const VkSampler currentDepthSampler,
                                const VkImageView waterVelocityView, const VkSampler waterVelocitySampler,
                                const VkImageView waterMetaView, const VkSampler waterMetaSampler,
                                const VkImageView waterSurfaceView, const VkSampler waterSurfaceSampler,
                                Assets::AssetManager &assets) {
        if (device == VK_NULL_HANDLE || currentView == VK_NULL_HANDLE || sampler == VK_NULL_HANDLE ||
            velocityView == VK_NULL_HANDLE || velocitySampler == VK_NULL_HANDLE ||
            currentDepthView == VK_NULL_HANDLE || currentDepthSampler == VK_NULL_HANDLE ||
            waterVelocityView == VK_NULL_HANDLE || waterVelocitySampler == VK_NULL_HANDLE ||
            waterMetaView == VK_NULL_HANDLE || waterMetaSampler == VK_NULL_HANDLE ||
            waterSurfaceView == VK_NULL_HANDLE || waterSurfaceSampler == VK_NULL_HANDLE) {
            throw std::invalid_argument("Temporal AA pass received incomplete resources");
        }
        destroy();
        device_ = device;
        try {
            VkDescriptorSetLayoutBinding bindings[11]{};
            for (std::uint32_t i = 0; i < 8; ++i) {
                bindings[i].binding = i;
                bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            }
            for (std::uint32_t i = 8; i < 11; ++i) {
                bindings[i].binding = i;
                bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            }
            VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            layoutInfo.bindingCount = std::size(bindings);
            layoutInfo.pBindings = bindings;
            if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &layout_) != VK_SUCCESS) {
                throw std::runtime_error("Could not create temporal AA descriptor layout");
            }
            for (HdrBuffer &image: history_) {
                image.create(physicalDevice, device_, extent, allocator, VK_FILTER_LINEAR, HdrBuffer::Format, true);
            }
            for (HdrBuffer &image: historyColor_) {
                image.create(physicalDevice, device_, extent, allocator, VK_FILTER_LINEAR, HdrBuffer::Format, true);
            }
            for (HdrBuffer &image: historyDepth_) {
                image.create(physicalDevice, device_, extent, allocator,
                             VK_FILTER_NEAREST, HdrBuffer::Format, true);
            }
            const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Settings)};
            VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            pipelineLayoutInfo.setLayoutCount = 1;
            pipelineLayoutInfo.pSetLayouts = &layout_;
            pipelineLayoutInfo.pushConstantRangeCount = 1;
            pipelineLayoutInfo.pPushConstantRanges = &push;
            if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) {
                throw std::runtime_error("Could not create temporal AA compute pipeline layout");
            }
            const auto shader = Vkutil::loadShaderModule(device_, assets, "shaders/temporal_aa.spv");
            VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                  VK_SHADER_STAGE_COMPUTE_BIT, shader.get(), "main", nullptr};
            pipelineInfo.layout = pipelineLayout_;
            if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_) != VK_SUCCESS) {
                throw std::runtime_error("Could not create temporal AA compute pipeline");
            }
            const VkDescriptorPoolSize sizes[] = {
                {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32},
                {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 12}
            };
            VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            poolInfo.maxSets = 4;
            poolInfo.poolSizeCount = std::size(sizes);
            poolInfo.pPoolSizes = sizes;
            if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &pool_) != VK_SUCCESS) {
                throw std::runtime_error("Could not create temporal AA descriptor pool");
            }
            VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            const std::array<VkDescriptorSetLayout, 4> layouts{layout_, layout_, layout_, layout_};
            allocation.descriptorPool = pool_;
            allocation.descriptorSetCount = 4;
            allocation.pSetLayouts = layouts.data();
            if (vkAllocateDescriptorSets(device_, &allocation, sets_.data()) != VK_SUCCESS) {
                throw std::runtime_error("Could not allocate temporal AA descriptor sets");
            }
            for (std::uint32_t i = 0; i < 2; ++i) {
                VkDescriptorImageInfo images[8] = {
                    {sampler, currentView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                    {historyColor_[i].sampler(), historyColor_[i].imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                    {velocitySampler, velocityView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                    {currentDepthSampler, currentDepthView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
                    {
                        historyDepth_[i].sampler(), historyDepth_[i].imageView(),
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                    },
                    // The shader's water branch is disabled for this set. These valid
                    // fallbacks keep bindings 5-7 legal while virtual water is unprepared.
                    {sampler, currentView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                    {sampler, currentView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                    {sampler, currentView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                };
                VkWriteDescriptorSet writes[8]{};
                for (std::uint32_t binding = 0; binding < 8; ++binding) {
                    writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                    writes[binding].dstSet = sets_[i];
                    writes[binding].dstBinding = binding;
                    writes[binding].descriptorCount = 1;
                    writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    writes[binding].pImageInfo = &images[binding];
                }
                vkUpdateDescriptorSets(device_, 8, writes, 0, nullptr);

                const std::uint32_t output = 1U - i;
                const VkDescriptorImageInfo outputs[] = {
                    {VK_NULL_HANDLE, history_[output].imageView(), VK_IMAGE_LAYOUT_GENERAL},
                    {VK_NULL_HANDLE, historyDepth_[output].imageView(), VK_IMAGE_LAYOUT_GENERAL},
                    {VK_NULL_HANDLE, historyColor_[output].imageView(), VK_IMAGE_LAYOUT_GENERAL}
                };
                VkWriteDescriptorSet outputWrites[3]{};
                for (std::uint32_t binding = 0; binding < 3; ++binding) {
                    outputWrites[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                    outputWrites[binding].dstSet = sets_[i];
                    outputWrites[binding].dstBinding = 8 + binding;
                    outputWrites[binding].descriptorCount = 1;
                    outputWrites[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    outputWrites[binding].pImageInfo = &outputs[binding];
                }
                vkUpdateDescriptorSets(device_, 3, outputWrites, 0, nullptr);

                constexpr std::uint32_t WaterSetOffset = 2;
                images[5] = {waterVelocitySampler, waterVelocityView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                images[6] = {waterMetaSampler, waterMetaView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                images[7] = {waterSurfaceSampler, waterSurfaceView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                for (std::uint32_t binding = 0; binding < 8; ++binding) {
                    writes[binding].dstSet = sets_[i + WaterSetOffset];
                }
                vkUpdateDescriptorSets(device_, 8, writes, 0, nullptr);
                for (auto &write: outputWrites) write.dstSet = sets_[i + WaterSetOffset];
                vkUpdateDescriptorSets(device_, 3, outputWrites, 0, nullptr);
            }
            reset();
        } catch (...) {
            destroy();
            throw;
        }
    }

    void TemporalAaPass::reset() noexcept {
        // Resetting accumulation does not change an image's Vulkan layout.  The
        // first record after a camera cut can safely overwrite the output history
        // while historyValid_ keeps the previous contents out of the resolve.
        // Re-marking already initialized images as UNDEFINED caused invalid layout
        // transitions on the next TAA frame.
        historyValid_ = false;
        historyIndex_ = 0;
        previousJitterX_ = previousJitterY_ = 0.0F;
    }

    void TemporalAaPass::initializeHistory(const VkCommandBuffer commandBuffer) {
        VkImageMemoryBarrier2 barriers[6]{};
        for (std::uint32_t i = 0; i < 2; ++i) {
            barriers[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            barriers[i].dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barriers[i].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barriers[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barriers[i].image = history_[i].image();
            barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            barriers[i + 2] = barriers[i];
            barriers[i + 2].image = historyDepth_[i].image();
            barriers[i + 4] = barriers[i];
            barriers[i + 4].image = historyColor_[i].image();
        }
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 6;
        dependency.pImageMemoryBarriers = barriers;
        vkCmdPipelineBarrier2(commandBuffer, &dependency);
        VkClearColorValue clear{};
        for (const HdrBuffer &image: history_) {
            vkCmdClearColorImage(commandBuffer, image.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                                 &barriers[0].subresourceRange);
        }
        for (const HdrBuffer &image: historyDepth_) {
            vkCmdClearColorImage(commandBuffer, image.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                                 &barriers[0].subresourceRange);
        }
        for (const HdrBuffer &image: historyColor_) {
            vkCmdClearColorImage(commandBuffer, image.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                                 &barriers[0].subresourceRange);
        }
        for (auto &barrier: barriers) {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        vkCmdPipelineBarrier2(commandBuffer, &dependency);
        initialized_ = true;
    }

    void TemporalAaPass::prepareHistory(const VkCommandBuffer commandBuffer) {
        if (!initialized_) {
            initializeHistory(commandBuffer);
        }
    }

    void TemporalAaPass::record(const VkCommandBuffer commandBuffer, const VkExtent2D extent,
                                const float currentJitterX, const float currentJitterY,
                                const float projectionA, const float projectionB) {
        const std::uint32_t output = 1U - historyIndex_;
        // The render graph owns the HDR output. Depth and bounded-color history
        // are internal storage images and need explicit layout transitions.
        VkImageMemoryBarrier2 internalBarriers[2]{};
        for (auto &barrier : internalBarriers) {
            barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        internalBarriers[0].image = historyDepth_[output].image();
        internalBarriers[1].image = historyColor_[output].image();
        VkDependencyInfo internalDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        internalDependency.imageMemoryBarrierCount = 2;
        internalDependency.pImageMemoryBarriers = internalBarriers;
        vkCmdPipelineBarrier2(commandBuffer, &internalDependency);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        constexpr std::uint32_t WaterSetOffset = 2;
        const VkDescriptorSet descriptorSet = sets_[historyIndex_ + (virtualWaterEnabled_ ? WaterSetOffset : 0U)];
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1,
                                &descriptorSet, 0, nullptr);
        // Reprojection, depth rejection and motion-scaled clipping gate the
        // stronger stable-pixel accumulation.
        const Settings settings{
            .currentJitterX = currentJitterX, .currentJitterY = currentJitterY, .previousJitterX = previousJitterX_,
            .previousJitterY = previousJitterY_, .historyWeight = historyValid_ ? 0.9375F : 0.0F,
            .inverseWidth = 1.0F / static_cast<float>(extent.width),
            .inverseHeight = 1.0F / static_cast<float>(extent.height),
            .padding = virtualWaterEnabled_ ? 1.0F : 0.0F,
            .projectionA = projectionA, .projectionB = projectionB
        };
        vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings),
                           &settings);
        vkCmdDispatch(commandBuffer, (extent.width + 15) / 16, (extent.height + 15) / 16, 1);
        for (auto &barrier : internalBarriers) {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        vkCmdPipelineBarrier2(commandBuffer, &internalDependency);
        historyIndex_ = output;
        historyValid_ = true;
        previousJitterX_ = currentJitterX;
        previousJitterY_ = currentJitterY;
    }

    void TemporalAaPass::destroy() noexcept {
        if (device_ != VK_NULL_HANDLE) {
            if (pipeline_) {
                vkDestroyPipeline(device_, pipeline_, nullptr);
            }
            if (pipelineLayout_) {
                vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
            }
            if (pool_) {
                vkDestroyDescriptorPool(device_, pool_, nullptr);
            }
            if (layout_) {
                vkDestroyDescriptorSetLayout(device_, layout_, nullptr);
            }
        }
        sets_.fill(VK_NULL_HANDLE);
        pool_ = VK_NULL_HANDLE;
        layout_ = VK_NULL_HANDLE;
        pipeline_ = VK_NULL_HANDLE;
        pipelineLayout_ = VK_NULL_HANDLE;
        for (HdrBuffer &image: history_) {
            image.destroy();
        }
        for (HdrBuffer &image: historyColor_) {
            image.destroy();
        }
        device_ = VK_NULL_HANDLE;
        initialized_ = false;
        for (HdrBuffer &image: historyDepth_) {
            image.destroy();
        }
        reset();
    }
} // namespace Engine
