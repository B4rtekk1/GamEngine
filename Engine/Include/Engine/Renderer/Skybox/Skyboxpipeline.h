#pragma once

#include <vulkan/vulkan.h>

namespace Engine {

class SkyboxPipeline final {
public:
    ~SkyboxPipeline();
    SkyboxPipeline() = default;
    SkyboxPipeline(const SkyboxPipeline&) = delete;
    SkyboxPipeline& operator=(const SkyboxPipeline&) = delete;

    void create(VkDevice device, VkRenderPass renderPass, VkFormat colorFormat,
                VkSampleCountFlagBits samples, VkDescriptorSetLayout descriptorSetLayout);
    void destroy() noexcept;
    [[nodiscard]] VkPipeline handle() const noexcept { return pipeline_; }
    [[nodiscard]] VkPipelineLayout layout() const noexcept { return layout_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

} // namespace Engine
