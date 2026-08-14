#include "Engine/Renderer/Skybox/Skybox.h"

#include <array>
#include <stdexcept>

namespace Engine {
namespace {
constexpr std::array<float, 108> kVertices = {
    -1,-1,-1,  1, 1,-1,  1,-1,-1,  1, 1,-1, -1,-1,-1, -1, 1,-1,
    -1,-1, 1,  1,-1, 1,  1, 1, 1,  1, 1, 1, -1, 1, 1, -1,-1, 1,
    -1, 1, 1, -1, 1,-1, -1,-1,-1, -1,-1,-1, -1,-1, 1, -1, 1, 1,
     1, 1, 1,  1,-1, 1,  1,-1,-1,  1,-1,-1,  1, 1,-1,  1, 1, 1,
    -1,-1,-1,  1,-1,-1,  1,-1, 1,  1,-1, 1, -1,-1, 1, -1,-1,-1,
    -1, 1,-1, -1, 1, 1,  1, 1, 1,  1, 1, 1,  1, 1,-1, -1, 1,-1,
};
constexpr std::array<std::array<uint8_t, 4>, 6> kFaceColours = {{
    {{68, 132, 205, 255}}, {{58, 110, 180, 255}}, {{105, 170, 225, 255}},
    {{12, 24, 55, 255}}, {{78, 142, 210, 255}}, {{52, 102, 170, 255}},
}};
}

Skybox::~Skybox() { destroy(); }

void Skybox::create(VkPhysicalDevice physicalDevice, VkDevice device, VkCommandPool commandPool, VkQueue queue,
                    VkRenderPass renderPass, VkFormat colorFormat, VkSampleCountFlagBits samples,
                    VkDescriptorSetLayout descriptorSetLayout, const std::vector<VkBuffer>& uniformBuffers,
                    VkDeviceSize uniformBufferRange, Assets::AssetManager& assets) {
    if (uniformBuffers.empty()) throw std::invalid_argument("Skybox requires camera uniform buffers");
    destroy(); device_ = device; descriptorSetLayout_ = descriptorSetLayout;
    try {
        vertexBuffer_.createDeviceLocal(physicalDevice, device_, kVertices.data(), sizeof(kVertices),
                                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, commandPool, queue);
        cubemap_.create(physicalDevice, device_, commandPool, queue, kFaceColours);
        pipeline_.create(device_, renderPass, colorFormat, samples, descriptorSetLayout_, assets);
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, static_cast<uint32_t>(uniformBuffers.size())}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, static_cast<uint32_t>(uniformBuffers.size())}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets = static_cast<uint32_t>(uniformBuffers.size()); pool.poolSizeCount = 2; pool.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(device_, &pool, nullptr, &descriptorPool_) != VK_SUCCESS) throw std::runtime_error("Could not create skybox descriptor pool");
        std::vector<VkDescriptorSetLayout> layouts(uniformBuffers.size(), descriptorSetLayout_);
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocation.descriptorPool = descriptorPool_; allocation.descriptorSetCount = static_cast<uint32_t>(layouts.size()); allocation.pSetLayouts = layouts.data();
        descriptorSets_.resize(uniformBuffers.size());
        if (vkAllocateDescriptorSets(device_, &allocation, descriptorSets_.data()) != VK_SUCCESS) throw std::runtime_error("Could not allocate skybox descriptor sets");
        const VkDescriptorImageInfo cubemapInfo{cubemap_.sampler(), cubemap_.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        for (size_t i = 0; i < uniformBuffers.size(); ++i) {
            VkDescriptorBufferInfo bufferInfo{uniformBuffers[i], 0, uniformBufferRange};
            VkWriteDescriptorSet writes[2]{};
            writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSets_[i], 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &bufferInfo};
            writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSets_[i], 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cubemapInfo};
            vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
        }
    } catch (...) { destroy(); throw; }
}

void Skybox::draw(VkCommandBuffer commandBuffer, uint32_t frameIndex) const {
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.layout(), 0, 1, &descriptorSets_.at(frameIndex), 0, nullptr);
    const VkBuffer buffer = vertexBuffer_.handle(); constexpr VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &buffer, &offset);
    vkCmdDraw(commandBuffer, 36, 1, 0, 0);
}

void Skybox::destroy() noexcept {
    if (device_ != VK_NULL_HANDLE && descriptorPool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
    descriptorSets_.clear(); descriptorPool_ = VK_NULL_HANDLE; pipeline_.destroy(); cubemap_.destroy(); vertexBuffer_.destroy(); descriptorSetLayout_ = VK_NULL_HANDLE; device_ = VK_NULL_HANDLE;
}
} // namespace Engine
