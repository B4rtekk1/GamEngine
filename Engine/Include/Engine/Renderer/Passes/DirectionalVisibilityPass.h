#pragma once

#include "Engine/Renderer/Vulkan/hdr_buffer.h"
#include "Engine/Renderer/Vulkan/shadow_map.h"

#include <array>
#include <span>
#include <vulkan/vulkan.h>

namespace Engine {
namespace Assets { class AssetManager; }

class DirectionalVisibilityPass final {
public:
    ~DirectionalVisibilityPass();
    void create(VkPhysicalDevice physical, VkDevice device, VkExtent2D extent, VmaAllocator allocator,
                Assets::AssetManager& assets, const ShadowMap& shadowMap,
                std::span<const VkBuffer> frameBuffers, std::span<const VkBuffer> pageTables);
    void destroy() noexcept;
    void record(VkCommandBuffer commandBuffer, std::uint32_t frameSlot,
                VkImageView depth, VkSampler depthSampler,
                VkImageView normals, VkSampler normalSampler);
    [[nodiscard]] VkImageView resultView(std::uint32_t frameSlot) const noexcept {
        return visibility_[frameSlot].imageView();
    }
    [[nodiscard]] VkSampler resultSampler(std::uint32_t frameSlot) const noexcept {
        return visibility_[frameSlot].sampler();
    }
    [[nodiscard]] VkExtent2D extent() const noexcept { return extent_; }

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkExtent2D extent_{};
    std::array<HdrBuffer, 2> visibility_{};
    VkDescriptorSetLayout layout_{VK_NULL_HANDLE};
    VkDescriptorPool pool_{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, 2> sets_{};
    VkPipelineLayout pipelineLayout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
};
} // namespace Engine
