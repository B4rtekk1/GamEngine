#pragma once

#include <vulkan\vulkan.h>

#include <cstdint>
#include <vector>

namespace Engine::Culling {
    class HiZBuffer {
    public:
        HiZBuffer() = default;

        HiZBuffer(
            VkPhysicalDevice physicalDevice,
            VkDevice device,
            std::uint32_t width,
            std::uint32_t height
        );

        ~HiZBuffer();

        HiZBuffer(const HiZBuffer &) = delete;

        HiZBuffer &operator=(const HiZBuffer &) = delete;

        HiZBuffer(HiZBuffer &&other) noexcept;

        HiZBuffer &operator=(HiZBuffer &&other) noexcept;

        void create(
            VkPhysicalDevice physicalDevice,
            VkDevice device,
            std::uint32_t width,
            std::uint32_t height
        );

        void destroy();

        [[nodiscard]] VkImage image() const noexcept { return m_image; }
        [[nodiscard]] VkImageView fullView() const noexcept { return m_fullView; }

        [[nodiscard]] VkImageView mipView(std::uint32_t mip) const;

        [[nodiscard]] VkSampler sampler() const noexcept { return m_sampler; }
        [[nodiscard]] std::uint32_t width() const noexcept { return m_width; }
        [[nodiscard]] std::uint32_t height() const noexcept { return m_height; }
        [[nodiscard]] std::uint32_t mipCount() const noexcept { return m_mipCount; }

        [[nodiscard]] VkExtent2D mipExtent(std::uint32_t mip) const;

    private:
        static std::uint32_t calculateMipCount(
            std::uint32_t width,
            std::uint32_t height
        );

        static std::uint32_t findMemoryType(
            VkPhysicalDevice physicalDevice,
            std::uint32_t typeFilter,
            VkMemoryPropertyFlags properties
        );

        void createImage();

        void createViews();

        void createSampler();

        VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
        VkDevice m_device{VK_NULL_HANDLE};

        VkImage m_image{VK_NULL_HANDLE};
        VkDeviceMemory m_memory{VK_NULL_HANDLE};
        VkImageView m_fullView{VK_NULL_HANDLE};
        VkSampler m_sampler{VK_NULL_HANDLE};

        std::vector<VkImageView> m_mipViews;

        std::uint32_t m_width{0};
        std::uint32_t m_height{0};
        std::uint32_t m_mipCount{0};
    };
    ;
}
