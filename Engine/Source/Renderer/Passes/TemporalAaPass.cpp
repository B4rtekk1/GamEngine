#include "Engine/Renderer/Passes/TemporalAaPass.h"

#include <stdexcept>

namespace Engine {
    namespace {
        struct Settings {
            float currentJitterX, currentJitterY, previousJitterX, previousJitterY;
            float historyWeight, inverseWidth, inverseHeight, padding;
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
            VkDescriptorSetLayoutBinding bindings[8]{};
            for (std::uint32_t i = 0; i < 8; ++i) {
                bindings[i].binding = i;
                bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            }
            VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            layoutInfo.bindingCount = std::size(bindings);
            layoutInfo.pBindings = bindings;
            if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &layout_) != VK_SUCCESS) {
                throw std::runtime_error("Could not create temporal AA descriptor layout");
            }
            for (HdrBuffer &image: history_) {
                image.create(physicalDevice, device_, extent, allocator);
            }
            for (HdrBuffer &image: historyDepth_) {
                image.create(physicalDevice, device_, extent, allocator,
                             VK_FILTER_NEAREST);
            }
            GraphicsPipelineOptions options{};
            options.colorFormat = HdrBuffer::Format;
            options.dynamicRendering = true;
            options.additionalColorFormat = HdrBuffer::Format;
            options.colorInitialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            options.colorFinalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            options.shader = "shaders/temporal_aa.spv";
            options.assetManager = &assets;
            options.pushConstantSize = sizeof(Settings);
            options.pushConstantStages = VK_SHADER_STAGE_FRAGMENT_BIT;
            options.cullMode = VK_CULL_MODE_NONE;
            options.depthTestEnable = VK_FALSE;
            options.depthWriteEnable = VK_FALSE;
            options.descriptorSetLayouts = {layout_};
            pipeline_.create(device_, options);
            VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32};
            VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            poolInfo.maxSets = 4;
            poolInfo.poolSizeCount = 1;
            poolInfo.pPoolSizes = &size;
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
                    {sampler, history_[i].imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
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

                images[5] = {waterVelocitySampler, waterVelocityView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                images[6] = {waterMetaSampler, waterMetaView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                images[7] = {waterSurfaceSampler, waterSurfaceView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                for (std::uint32_t binding = 0; binding < 8; ++binding) {
                    constexpr std::uint32_t WaterSetOffset = 2;
                    writes[binding].dstSet = sets_[i + WaterSetOffset];
                }
                vkUpdateDescriptorSets(device_, 8, writes, 0, nullptr);
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
        VkImageMemoryBarrier2 barriers[4]{};
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
        }
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 4;
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
        for (auto &barrier: barriers) {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
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
                                const float currentJitterX, const float currentJitterY) {
        const std::uint32_t output = 1U - historyIndex_;
        // The render graph owns history_, while the matching depth history is an
        // internal attachment. Dynamic rendering has no render-pass finalLayout,
        // so transition that internal image explicitly.
        VkImageMemoryBarrier2 depthToAttachment{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        depthToAttachment.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        depthToAttachment.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        depthToAttachment.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        depthToAttachment.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        depthToAttachment.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        depthToAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        depthToAttachment.image = historyDepth_[output].image();
        depthToAttachment.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo depthDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        depthDependency.imageMemoryBarrierCount = 1;
        depthDependency.pImageMemoryBarriers = &depthToAttachment;
        vkCmdPipelineBarrier2(commandBuffer, &depthDependency);
        std::array colors{
            VkRenderingAttachmentInfo{
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView = history_[output].imageView(),
                .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE
            },
            VkRenderingAttachmentInfo{
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView = historyDepth_[output].imageView(),
                .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE
            }
        };
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea.extent = extent;
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = static_cast<std::uint32_t>(colors.size());
        rendering.pColorAttachments = colors.data();
        vkCmdBeginRendering(commandBuffer, &rendering);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
        constexpr std::uint32_t WaterSetOffset = 2;
        const VkDescriptorSet descriptorSet = sets_[historyIndex_ + (virtualWaterEnabled_ ? WaterSetOffset : 0U)];
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.layout(), 0, 1,
                                &descriptorSet, 0, nullptr);
        // Keep most of the stable history while retaining enough current-frame
        // contribution to limit ghosting on moving or newly revealed geometry.
        const Settings settings{
            .currentJitterX = currentJitterX, .currentJitterY = currentJitterY, .previousJitterX = previousJitterX_,
            .previousJitterY = previousJitterY_, .historyWeight = historyValid_ ? 0.82F : 0.0F,
            .inverseWidth = 1.0F / static_cast<float>(extent.width),
            .inverseHeight = 1.0F / static_cast<float>(extent.height),
            .padding = virtualWaterEnabled_ ? 1.0F : 0.0F
        };
        vkCmdPushConstants(commandBuffer, pipeline_.layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(settings),
                           &settings);
        const VkViewport viewport{0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height), 0, 1};
        const VkRect2D scissor{{0, 0}, extent};
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
        vkCmdEndRendering(commandBuffer);
        depthToAttachment.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        depthToAttachment.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        depthToAttachment.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        depthToAttachment.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        depthToAttachment.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        depthToAttachment.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier2(commandBuffer, &depthDependency);
        historyIndex_ = output;
        historyValid_ = true;
        previousJitterX_ = currentJitterX;
        previousJitterY_ = currentJitterY;
    }

    void TemporalAaPass::destroy() noexcept {
        if (device_ != VK_NULL_HANDLE) {
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
        pipeline_.destroy();
        for (HdrBuffer &image: history_) {
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
