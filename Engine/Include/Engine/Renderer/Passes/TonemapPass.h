#pragma once

#include "Engine/Renderer/Vulkan/graphics_pipeline.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace Engine {
namespace Assets { class AssetManager; }

class TonemapPass final {
public:
    ~TonemapPass();

    TonemapPass() = default;
    TonemapPass(const TonemapPass&) = delete;
    TonemapPass& operator=(const TonemapPass&) = delete;

    void create(VkDevice device, VkFormat swapchainFormat, VkExtent2D extent,
                const std::vector<VkImageView>& swapchainViews,
                VkImageView hdrView, VkSampler hdrSampler,
                Assets::AssetManager& assets);
    void destroy() noexcept;

    void record(VkCommandBuffer commandBuffer, std::uint32_t imageIndex,
                VkExtent2D extent, float exposure = 0.0f) const;

private:
    VkDevice device_ = VK_NULL_HANDLE;
    GraphicsPipeline pipeline_;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;
    bool manualGamma_ = false;
};

} // namespace Engine
