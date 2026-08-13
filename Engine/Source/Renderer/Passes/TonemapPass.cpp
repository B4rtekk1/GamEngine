#include "Engine/Renderer/Passes/TonemapPass.h"

#include <stdexcept>

namespace Engine {
namespace {

struct TonemapSettings {
    float exposure;
    float applyGamma;
};

bool isSrgbFormat(const VkFormat format) {
    return format == VK_FORMAT_R8G8B8A8_SRGB ||
           format == VK_FORMAT_B8G8R8A8_SRGB ||
           format == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

} // namespace

TonemapPass::~TonemapPass() {
    destroy();
}

void TonemapPass::create(const VkDevice device, const VkFormat swapchainFormat,
                         const VkExtent2D extent,
                         const std::vector<VkImageView>& swapchainViews,
                         const VkImageView hdrView, const VkSampler hdrSampler) {
    if (device == VK_NULL_HANDLE || swapchainFormat == VK_FORMAT_UNDEFINED ||
        swapchainViews.empty() || hdrView == VK_NULL_HANDLE || hdrSampler == VK_NULL_HANDLE) {
        throw std::invalid_argument("Tonemap pass received incomplete resources");
    }

    destroy();
    device_ = device;
    manualGamma_ = !isSrgbFormat(swapchainFormat);
    try {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &binding;
        if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr,
                                        &descriptorSetLayout_) != VK_SUCCESS) {
            throw std::runtime_error("Could not create tonemap descriptor layout");
        }

        GraphicsPipelineOptions options{};
        options.colorFormat = swapchainFormat;
        options.colorFinalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        options.vertexShader = "shaders/tonemap.vert.spv";
        options.fragmentShader = "shaders/tonemap.frag.spv";
        options.pushConstantSize = sizeof(TonemapSettings);
        options.pushConstantStages = VK_SHADER_STAGE_FRAGMENT_BIT;
        options.cullMode = VK_CULL_MODE_NONE;
        options.depthTestEnable = VK_FALSE;
        options.depthWriteEnable = VK_FALSE;
        options.descriptorSetLayouts = {descriptorSetLayout_};
        pipeline_.create(device_, options);

        framebuffers_.resize(swapchainViews.size());
        for (std::size_t index = 0; index < swapchainViews.size(); ++index) {
            VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            framebufferInfo.renderPass = pipeline_.renderPass();
            framebufferInfo.attachmentCount = 1;
            framebufferInfo.pAttachments = &swapchainViews[index];
            framebufferInfo.width = extent.width;
            framebufferInfo.height = extent.height;
            framebufferInfo.layers = 1;
            if (vkCreateFramebuffer(device_, &framebufferInfo, nullptr,
                                    &framebuffers_[index]) != VK_SUCCESS) {
                throw std::runtime_error("Could not create tonemap framebuffer");
            }
        }

        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS) {
            throw std::runtime_error("Could not create tonemap descriptor pool");
        }

        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = descriptorPool_;
        allocation.descriptorSetCount = 1;
        allocation.pSetLayouts = &descriptorSetLayout_;
        if (vkAllocateDescriptorSets(device_, &allocation, &descriptorSet_) != VK_SUCCESS) {
            throw std::runtime_error("Could not allocate tonemap descriptor set");
        }

        VkDescriptorImageInfo imageInfo{hdrSampler, hdrView,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = descriptorSet_;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    } catch (...) {
        destroy();
        throw;
    }
}

void TonemapPass::destroy() noexcept {
    if (device_ != VK_NULL_HANDLE) {
        for (const VkFramebuffer framebuffer : framebuffers_) {
            if (framebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device_, framebuffer, nullptr);
            }
        }
        if (descriptorPool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        }
    }
    framebuffers_.clear();
    descriptorSet_ = VK_NULL_HANDLE;
    descriptorPool_ = VK_NULL_HANDLE;
    pipeline_.destroy();
    if (device_ != VK_NULL_HANDLE && descriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
    }
    descriptorSetLayout_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
}

void TonemapPass::record(const VkCommandBuffer commandBuffer,
                         const std::uint32_t imageIndex, const VkExtent2D extent,
                         const float exposure) const {
    VkRenderPassBeginInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    passInfo.renderPass = pipeline_.renderPass();
    passInfo.framebuffer = framebuffers_.at(imageIndex);
    passInfo.renderArea.extent = extent;
    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    passInfo.clearValueCount = 1;
    passInfo.pClearValues = &clear;
    vkCmdBeginRenderPass(commandBuffer, &passInfo, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline_.layout(), 0, 1, &descriptorSet_, 0, nullptr);
    const TonemapSettings settings{exposure, manualGamma_ ? 1.0f : 0.0f};
    vkCmdPushConstants(commandBuffer, pipeline_.layout(), VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(settings), &settings);
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width),
                              static_cast<float>(extent.height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    vkCmdEndRenderPass(commandBuffer);
}

} // namespace Engine
