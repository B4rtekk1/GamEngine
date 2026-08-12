#pragma once

#include "../Textures/Cubemap.h"
#include "Skyboxpipeline.h"
#include "Engine/Renderer/Vulkan/buffer.h"

#include <vulkan/vulkan.h>

#include <vector>

namespace Engine {

class Skybox final {
public:
    ~Skybox();
    Skybox() = default;
    Skybox(const Skybox&) = delete;
    Skybox& operator=(const Skybox&) = delete;

    void create(VkPhysicalDevice physicalDevice, VkDevice device, VkCommandPool commandPool,
                VkQueue queue, VkRenderPass renderPass, VkFormat colorFormat,
                VkSampleCountFlagBits samples, VkDescriptorSetLayout descriptorSetLayout,
                const std::vector<VkBuffer>& uniformBuffers, VkDeviceSize uniformBufferRange);
    void draw(VkCommandBuffer commandBuffer, uint32_t frameIndex) const;
    void destroy() noexcept;

private:
    Buffer vertexBuffer_;
    Cubemap cubemap_;
    SkyboxPipeline pipeline_;
    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptorSets_;
};

} // namespace Engine
