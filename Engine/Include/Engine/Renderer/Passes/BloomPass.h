#pragma once

#include "Engine/Renderer/Vulkan/graphics_pipeline.h"
#include "Engine/Renderer/Vulkan/hdr_buffer.h"

#include <array>
#include <cstdint>
#include <vulkan/vulkan.h>

namespace Engine {
    namespace Assets { class AssetManager; }

    /** Extracts and filters bright linear-HDR pixels before tone mapping. */
    class BloomPass final {
    public:
        ~BloomPass();
        BloomPass() = default;
        BloomPass(const BloomPass&) = delete;
        BloomPass& operator=(const BloomPass&) = delete;
        void create(VkPhysicalDevice physicalDevice, VkDevice device, VkExtent2D sourceExtent,
                    VmaAllocator allocator, Assets::AssetManager& assets);
        void destroy() noexcept;
        void record(VkCommandBuffer commandBuffer, VkImageView source, VkSampler sourceSampler,
                    std::uint32_t frameIndex);
        [[nodiscard]] VkImageView resultView() const noexcept { return result_.imageView(); }
        [[nodiscard]] VkImage resultImage() const noexcept { return result_.image(); }
        [[nodiscard]] bool initialized() const noexcept { return initialized_; }
        [[nodiscard]] VkSampler resultSampler() const noexcept { return result_.sampler(); }
    private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkExtent2D extent_{};
        HdrBuffer result_;
        GraphicsPipeline pipeline_;
        VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
        VkDescriptorPool pool_ = VK_NULL_HANDLE;
        static constexpr std::uint32_t FramesInFlight = 2;
        std::array<VkDescriptorSet, FramesInFlight> sets_{};
        VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
        bool initialized_ = false;
    };
}
