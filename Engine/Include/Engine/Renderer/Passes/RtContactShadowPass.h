#pragma once

#include "Engine/Renderer/RenderConfig.h"
#include "Engine/Renderer/Vulkan/hdr_buffer.h"

#include <array>
#include <span>
#include <vulkan/vulkan.h>

namespace Engine {
namespace Assets { class AssetManager; }

class RtContactShadowPass final {
public:
    ~RtContactShadowPass();
    RtContactShadowPass() = default;
    RtContactShadowPass(const RtContactShadowPass&) = delete;
    RtContactShadowPass& operator=(const RtContactShadowPass&) = delete;

    void create(VkPhysicalDevice physicalDevice, VkDevice device, VkExtent2D extent,
                VmaAllocator allocator, Assets::AssetManager& assets,
                std::span<const VkBuffer> frameUniformBuffers);
    void destroy() noexcept;
    void record(VkCommandBuffer commandBuffer, std::uint32_t frameSlot,
                VkAccelerationStructureKHR tlas, VkImageView depth, VkSampler depthSampler,
                VkImageView normals, VkSampler normalSampler,
                VkImageView directionalVisibility, VkSampler directionalSampler,
                const RtContactShadowSettings& settings);
    [[nodiscard]] VkImageView resultView() const noexcept { return visibility_.imageView(); }
    [[nodiscard]] VkSampler resultSampler() const noexcept { return visibility_.sampler(); }
    [[nodiscard]] VkExtent2D extent() const noexcept { return extent_; }

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkExtent2D extent_{};
    HdrBuffer visibility_;
    VkDescriptorSetLayout layout_{VK_NULL_HANDLE};
    VkDescriptorPool pool_{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, 2> sets_{};
    VkPipelineLayout pipelineLayout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
};
} // namespace Engine
