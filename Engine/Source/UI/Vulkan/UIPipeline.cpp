#include <Engine/UI/Vulkan/UIPipeline.h>

#include <Engine/UI/UIVertex.h>

#include <array>
#include <cstddef>
#include <stdexcept>

namespace Engine::UI {
namespace {

struct ScreenData {
    float inverseWidth;
    float inverseHeight;
};

} // namespace

UIPipeline::~UIPipeline() {
    destroy();
}

void UIPipeline::create(const VkDevice device, const VkFormat colorFormat,
                        const VkExtent2D extent,
                        const std::vector<VkImageView>& imageViews,
                        Engine::Assets::AssetManager& assets,
                        const VkImageView fontAtlasView,
                        const VkSampler fontAtlasSampler) {
    if (device == VK_NULL_HANDLE || colorFormat == VK_FORMAT_UNDEFINED ||
        extent.width == 0 || extent.height == 0 || imageViews.empty()) {
        throw std::invalid_argument("UI pipeline received incomplete resources");
    }
    if ((fontAtlasView == VK_NULL_HANDLE) != (fontAtlasSampler == VK_NULL_HANDLE)) {
        throw std::invalid_argument("UI font atlas requires both an image view and sampler");
    }

    destroy();
    device_ = device;
    try {
        const VkVertexInputBindingDescription binding{
            0, sizeof(UIVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        const std::array attributes{
            VkVertexInputAttributeDescription{
                0, 0, VK_FORMAT_R32G32_SFLOAT,
                static_cast<std::uint32_t>(offsetof(UIVertex, position))},
            VkVertexInputAttributeDescription{
                1, 0, VK_FORMAT_R32G32_SFLOAT,
                static_cast<std::uint32_t>(offsetof(UIVertex, uv))},
            VkVertexInputAttributeDescription{
                2, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                static_cast<std::uint32_t>(offsetof(UIVertex, color))},
            VkVertexInputAttributeDescription{
                3, 0, VK_FORMAT_R32_SFLOAT,
                static_cast<std::uint32_t>(offsetof(UIVertex, textSample))},
        };

        VkDescriptorSetLayoutBinding fontBinding{
            0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
            VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &fontBinding;
        if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr,
                                         &descriptorSetLayout_) != VK_SUCCESS) {
            throw std::runtime_error("Could not create UI font descriptor set layout");
        }

        if (fontAtlasView != VK_NULL_HANDLE) {
            VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
            VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            poolInfo.maxSets = 1;
            poolInfo.poolSizeCount = 1;
            poolInfo.pPoolSizes = &poolSize;
            if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS) {
                throw std::runtime_error("Could not create UI font descriptor pool");
            }
            VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            allocateInfo.descriptorPool = descriptorPool_;
            allocateInfo.descriptorSetCount = 1;
            allocateInfo.pSetLayouts = &descriptorSetLayout_;
            if (vkAllocateDescriptorSets(device_, &allocateInfo, &descriptorSet_) != VK_SUCCESS) {
                throw std::runtime_error("Could not allocate UI font descriptor set");
            }
            const VkDescriptorImageInfo imageInfo{
                fontAtlasSampler, fontAtlasView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            const VkWriteDescriptorSet write{
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet_, 0, 0, 1,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imageInfo, nullptr, nullptr};
            vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        }

        GraphicsPipelineOptions options{};
        options.colorFormat = colorFormat;
        options.colorLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        options.colorInitialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        options.colorFinalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        options.shader = "shaders/canvas.spv";
        options.assetManager = &assets;
        options.pushConstantSize = sizeof(ScreenData);
        options.pushConstantStages = VK_SHADER_STAGE_VERTEX_BIT;
        options.cullMode = VK_CULL_MODE_NONE;
        options.depthTestEnable = VK_FALSE;
        options.depthWriteEnable = VK_FALSE;
        options.alphaBlendEnable = VK_TRUE;
        options.vertexBindings = {binding};
        options.vertexAttributes.assign(attributes.begin(), attributes.end());
        options.descriptorSetLayouts = {descriptorSetLayout_};
        pipeline_.create(device_, options);

        framebuffers_.resize(imageViews.size());
        for (std::size_t index = 0; index < imageViews.size(); ++index) {
            VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            info.renderPass = pipeline_.renderPass();
            info.attachmentCount = 1;
            info.pAttachments = &imageViews[index];
            info.width = extent.width;
            info.height = extent.height;
            info.layers = 1;
            if (vkCreateFramebuffer(device_, &info, nullptr,
                                    &framebuffers_[index]) != VK_SUCCESS) {
                throw std::runtime_error("Could not create UI framebuffer");
            }
        }
    } catch (...) {
        destroy();
        throw;
    }
}

void UIPipeline::destroy() noexcept {
    if (device_ != VK_NULL_HANDLE) {
        for (const VkFramebuffer framebuffer : framebuffers_) {
            if (framebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device_, framebuffer, nullptr);
            }
        }
        if (descriptorPool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        }
        if (descriptorSetLayout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
        }
    }
    framebuffers_.clear();
    descriptorSet_ = VK_NULL_HANDLE;
    descriptorPool_ = VK_NULL_HANDLE;
    descriptorSetLayout_ = VK_NULL_HANDLE;
    pipeline_.destroy();
    device_ = VK_NULL_HANDLE;
}

void UIPipeline::record(const VkCommandBuffer commandBuffer,
                        const std::uint32_t imageIndex, const VkExtent2D extent,
                        const VkBuffer vertexBuffer, const VkBuffer indexBuffer,
                        const std::uint32_t indexCount) const {
    VkRenderPassBeginInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    passInfo.renderPass = pipeline_.renderPass();
    passInfo.framebuffer = framebuffers_.at(imageIndex);
    passInfo.renderArea.extent = extent;
    vkCmdBeginRenderPass(commandBuffer, &passInfo, VK_SUBPASS_CONTENTS_INLINE);

    if (indexCount != 0) {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipeline_.handle());
        if (descriptorSet_ != VK_NULL_HANDLE) {
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                     pipeline_.layout(), 0, 1, &descriptorSet_, 0, nullptr);
        }
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &offset);
        vkCmdBindIndexBuffer(commandBuffer, indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        const ScreenData screen{
            1.0F / static_cast<float>(extent.width),
            1.0F / static_cast<float>(extent.height)};
        vkCmdPushConstants(commandBuffer, pipeline_.layout(),
                           VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(screen), &screen);
        const VkViewport viewport{0.0F, 0.0F, static_cast<float>(extent.width),
                                  static_cast<float>(extent.height), 0.0F, 1.0F};
        const VkRect2D scissor{{0, 0}, extent};
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
        vkCmdDrawIndexed(commandBuffer, indexCount, 1, 0, 0, 0);
    }

    vkCmdEndRenderPass(commandBuffer);
}

} // namespace Engine::UI
