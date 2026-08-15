#pragma once

#include <Engine/Renderer/Vulkan/graphics_pipeline.h>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace Engine { namespace Assets { class AssetManager; } }

namespace Engine::UI {

class UIPipeline final {
public:
    ~UIPipeline();

    UIPipeline() = default;
    UIPipeline(const UIPipeline&) = delete;
    UIPipeline& operator=(const UIPipeline&) = delete;

    void create(VkDevice device, VkFormat colorFormat, VkExtent2D extent,
                const std::vector<VkImageView>& imageViews,
                Engine::Assets::AssetManager& assets,
                VkImageView fontAtlasView = VK_NULL_HANDLE,
                VkSampler fontAtlasSampler = VK_NULL_HANDLE);
    void destroy() noexcept;

    void record(VkCommandBuffer commandBuffer, std::uint32_t imageIndex,
                VkExtent2D extent, VkBuffer vertexBuffer, VkBuffer indexBuffer,
                std::uint32_t indexCount) const;

private:
    VkDevice device_ = VK_NULL_HANDLE;
    GraphicsPipeline pipeline_;
    std::vector<VkFramebuffer> framebuffers_;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
};

} // namespace Engine::UI
