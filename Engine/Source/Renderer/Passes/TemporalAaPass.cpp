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
                            Assets::AssetManager& assets) {
    if (device == VK_NULL_HANDLE || currentView == VK_NULL_HANDLE || sampler == VK_NULL_HANDLE ||
        velocityView == VK_NULL_HANDLE || velocitySampler == VK_NULL_HANDLE)
        throw std::invalid_argument("Temporal AA pass received incomplete resources");
    destroy(); device_ = device;
    try {
        VkDescriptorSetLayoutBinding bindings[3]{};
        for (std::uint32_t i = 0; i < 3; ++i) {
            bindings[i].binding = i; bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = 1; bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = std::size(bindings); layoutInfo.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &layout_) != VK_SUCCESS)
            throw std::runtime_error("Could not create temporal AA descriptor layout");
        for (HdrBuffer& image : history_) image.create(physicalDevice, device_, extent, allocator);
        GraphicsPipelineOptions options{};
        options.colorFormat = HdrBuffer::Format;
        options.colorInitialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        options.colorFinalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        options.shader = "shaders/temporal_aa.spv"; options.assetManager = &assets;
        options.pushConstantSize = sizeof(Settings); options.pushConstantStages = VK_SHADER_STAGE_FRAGMENT_BIT;
        options.cullMode = VK_CULL_MODE_NONE; options.depthTestEnable = VK_FALSE; options.depthWriteEnable = VK_FALSE;
        options.descriptorSetLayouts = {layout_}; pipeline_.create(device_, options);
        for (std::size_t i = 0; i < framebuffers_.size(); ++i) {
            const VkImageView view = history_[i].imageView();
            VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            info.renderPass = pipeline_.renderPass(); info.attachmentCount = 1; info.pAttachments = &view;
            info.width = extent.width; info.height = extent.height; info.layers = 1;
            if (vkCreateFramebuffer(device_, &info, nullptr, &framebuffers_[i]) != VK_SUCCESS)
                throw std::runtime_error("Could not create temporal AA framebuffer");
        }
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 2; poolInfo.poolSizeCount = 1; poolInfo.pPoolSizes = &size;
        if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &pool_) != VK_SUCCESS)
            throw std::runtime_error("Could not create temporal AA descriptor pool");
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        const std::array<VkDescriptorSetLayout, 2> layouts{layout_, layout_};
        allocation.descriptorPool = pool_; allocation.descriptorSetCount = 2;
        allocation.pSetLayouts = layouts.data();
        if (vkAllocateDescriptorSets(device_, &allocation, sets_.data()) != VK_SUCCESS)
            throw std::runtime_error("Could not allocate temporal AA descriptor sets");
        for (std::uint32_t i = 0; i < 2; ++i) {
            VkDescriptorImageInfo images[3] = {{sampler, currentView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                               {sampler, history_[i].imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                               {velocitySampler, velocityView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
            VkWriteDescriptorSet writes[3]{};
            for (std::uint32_t binding = 0; binding < 3; ++binding) {
                writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[binding].dstSet = sets_[i];
                writes[binding].dstBinding = binding; writes[binding].descriptorCount = 1;
                writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[binding].pImageInfo = &images[binding];
            }
            vkUpdateDescriptorSets(device_, 3, writes, 0, nullptr);
        }
        reset();
    } catch (...) { destroy(); throw; }
}

void TemporalAaPass::reset() noexcept { initialized_ = false; historyValid_ = false; historyIndex_ = 0; previousJitterX_ = previousJitterY_ = 0.0F; }

void TemporalAaPass::initializeHistory(const VkCommandBuffer commandBuffer) {
    VkImageMemoryBarrier2 barriers[2]{};
    for (std::uint32_t i = 0; i < 2; ++i) {
        barriers[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}; barriers[i].dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barriers[i].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT; barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barriers[i].image = history_[i].image();
        barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dependency.imageMemoryBarrierCount = 2; dependency.pImageMemoryBarriers = barriers;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
    VkClearColorValue clear{};
    for (const HdrBuffer& image : history_) vkCmdClearColorImage(commandBuffer, image.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &barriers[0].subresourceRange);
    for (auto& barrier : barriers) { barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT; barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT; barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT; barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT; barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
    vkCmdPipelineBarrier2(commandBuffer, &dependency); initialized_ = true;
}

void TemporalAaPass::record(const VkCommandBuffer commandBuffer, const VkExtent2D extent, const float currentJitterX, const float currentJitterY) {
    if (!initialized_) initializeHistory(commandBuffer);
    const std::uint32_t output = 1U - historyIndex_;
    VkClearValue clearValue{};
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; begin.renderPass = pipeline_.renderPass(); begin.framebuffer = framebuffers_[output]; begin.renderArea.extent = extent;
    begin.clearValueCount = 1;
    begin.pClearValues = &clearValue;
    vkCmdBeginRenderPass(commandBuffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.layout(), 0, 1, &sets_[historyIndex_], 0, nullptr);
    // A 16-frame accumulation is too soft for this renderer's jitter pattern.
    // Retain temporal stability while letting the current frame restore detail.
    const Settings settings{currentJitterX, currentJitterY, previousJitterX_, previousJitterY_, historyValid_ ? 0.80F : 0.0F,
                            1.0F / static_cast<float>(extent.width), 1.0F / static_cast<float>(extent.height), 0.0F};
    vkCmdPushConstants(commandBuffer, pipeline_.layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(settings), &settings);
    const VkViewport viewport{0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height), 0, 1}; const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport); vkCmdSetScissor(commandBuffer, 0, 1, &scissor); vkCmdDraw(commandBuffer, 3, 1, 0, 0); vkCmdEndRenderPass(commandBuffer);
    historyIndex_ = output; historyValid_ = true; previousJitterX_ = currentJitterX; previousJitterY_ = currentJitterY;
}

void TemporalAaPass::destroy() noexcept {
    if (device_ != VK_NULL_HANDLE) { for (auto framebuffer : framebuffers_) if (framebuffer) vkDestroyFramebuffer(device_, framebuffer, nullptr); if (pool_) vkDestroyDescriptorPool(device_, pool_, nullptr); if (layout_) vkDestroyDescriptorSetLayout(device_, layout_, nullptr); }
    framebuffers_.fill(VK_NULL_HANDLE); sets_.fill(VK_NULL_HANDLE); pool_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE; pipeline_.destroy(); for (HdrBuffer& image : history_) image.destroy(); device_ = VK_NULL_HANDLE; reset();
}
} // namespace Engine
