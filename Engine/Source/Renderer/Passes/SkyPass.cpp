#include "Engine/Renderer/Passes/SkyPass.h"

#include <stdexcept>

namespace Engine {

SkyPass::~SkyPass() {
    destroy();
}

void SkyPass::create(const VkPhysicalDevice physicalDevice, const VkDevice device,
                     const VkCommandPool commandPool, const VkQueue queue,
                     const VkRenderPass renderPass, const VkFormat colorFormat,
                     const VkSampleCountFlagBits samples,
                     const std::vector<VkBuffer>& uniformBuffers,
                     const VkDeviceSize uniformBufferRange) {
    destroy();
    device_ = device;
    try {
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                       VK_SHADER_STAGE_VERTEX_BIT, nullptr};
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        const VkDescriptorSetLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0,
            2, bindings};
        if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr,
                                        &descriptorSetLayout_) != VK_SUCCESS) {
            throw std::runtime_error("Could not create sky descriptor-set layout");
        }
        skybox_.create(physicalDevice, device_, commandPool, queue, renderPass,
                       colorFormat, samples, descriptorSetLayout_, uniformBuffers,
                       uniformBufferRange);
    } catch (...) {
        destroy();
        throw;
    }
}

void SkyPass::destroy() noexcept {
    skybox_.destroy();
    if (device_ != VK_NULL_HANDLE && descriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
    }
    descriptorSetLayout_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
}

void SkyPass::record(const VkCommandBuffer commandBuffer,
                     const std::uint32_t frameIndex) const {
    skybox_.draw(commandBuffer, frameIndex);
}

} // namespace Engine
