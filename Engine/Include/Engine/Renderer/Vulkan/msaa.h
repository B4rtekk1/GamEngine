#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace Engine {
    class MsaaResources {
    public:
        MsaaResources() = default;

        ~MsaaResources() noexcept {
            destroy();
        }

        MsaaResources(const MsaaResources &) = delete;

        MsaaResources &operator=(const MsaaResources &) = delete;

        void initialize(
            VkPhysicalDevice physicalDevice,
            VkDevice device,
            VkSampleCountFlagBits preferredSamples = VK_SAMPLE_COUNT_4_BIT,
            VmaAllocator allocator = VK_NULL_HANDLE
        );

        void create(VkExtent2D extent, VkFormat colorFormat);

        void destroy() noexcept;

        [[nodiscard]] VkSampleCountFlagBits sampleCount() const { return sampleCount_; }
        [[nodiscard]] bool enabled() const { return sampleCount_ != VK_SAMPLE_COUNT_1_BIT; }
        [[nodiscard]] VkImageView colorImageView() const { return colorImageView_; }

    private:
        VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
        VkDevice device_ = VK_NULL_HANDLE;
        VkSampleCountFlagBits sampleCount_ = VK_SAMPLE_COUNT_1_BIT;

        VkImage colorImage_ = VK_NULL_HANDLE;
        VmaAllocation colorImageAllocation_ = VK_NULL_HANDLE;
        VmaAllocator allocator_ = VK_NULL_HANDLE;
        VkImageView colorImageView_ = VK_NULL_HANDLE;

        static VkSampleCountFlagBits chooseSampleCount(
            VkPhysicalDevice physicalDevice,
            VkSampleCountFlagBits preferredSamples);

    };
} // namespace Engine
