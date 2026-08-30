#pragma once

/**
 * @file HiZBuffer.h
 * @brief Declares the hierarchical-Z depth pyramid used for occlusion culling.
 */

#include <vulkan\vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <vector>

namespace Engine::Culling {
    /**
     * @brief Owns a Vulkan hierarchical-Z depth image and its mip views.
     *
     * Each mip level represents a progressively lower-resolution depth image,
     * allowing GPU culling to test object visibility at an appropriate scale.
     */
    class HiZBuffer {
    public:
        /// Creates an empty Hi-Z buffer wrapper.
        HiZBuffer() = default;

        /**
         * @brief Creates a Hi-Z buffer for the specified dimensions.
         * @param physicalDevice Physical device used for resource creation.
         * @param device Logical device used to create Vulkan resources.
         * @param width Base-level image width in pixels.
         * @param height Base-level image height in pixels.
         */
        HiZBuffer(
            VkPhysicalDevice physicalDevice,
            VkDevice device,
            std::uint32_t width,
            std::uint32_t height,
            VmaAllocator allocator
        );

        /// Destroys the owned Vulkan image, views, sampler and memory.
        ~HiZBuffer();

        /// Copy construction is disabled because the object owns Vulkan resources.
        HiZBuffer(const HiZBuffer &) = delete;

        /// Copy assignment is disabled because the object owns Vulkan resources.
        HiZBuffer &operator=(const HiZBuffer &) = delete;

        /// Transfers ownership from another Hi-Z buffer.
        HiZBuffer(HiZBuffer &&other) noexcept;

        /// Transfers ownership from another Hi-Z buffer.
        HiZBuffer &operator=(HiZBuffer &&other) noexcept;

        /**
         * @brief Creates or recreates the Hi-Z image and its views.
         * @param physicalDevice Physical device used for resource creation.
         * @param device Logical device used to create Vulkan resources.
         * @param width Base-level image width in pixels.
         * @param height Base-level image height in pixels.
         */
        void create(
            VkPhysicalDevice physicalDevice,
            VkDevice device,
            std::uint32_t width,
            std::uint32_t height,
            VmaAllocator allocator
        );

        /// Releases all owned Vulkan resources.
        void destroy();

        /** @brief Returns the complete Hi-Z image handle. */
        [[nodiscard]] VkImage image() const noexcept { return m_image; }
        /** @brief Returns the view covering all Hi-Z mip levels. */
        [[nodiscard]] VkImageView fullView() const noexcept { return m_fullView; }

        /**
         * @brief Returns the view for one Hi-Z mip level.
         * @param mip Zero-based mip-level index.
         * @return Image view for the requested mip level.
         */
        [[nodiscard]] VkImageView mipView(std::uint32_t mip) const;

        /** @brief Returns the sampler used to sample the depth pyramid. */
        [[nodiscard]] VkSampler sampler() const noexcept { return m_sampler; }
        /** @brief Returns the base-level image width in pixels. */
        [[nodiscard]] std::uint32_t width() const noexcept { return m_width; }
        /** @brief Returns the base-level image height in pixels. */
        [[nodiscard]] std::uint32_t height() const noexcept { return m_height; }
        /** @brief Returns the number of mip levels in the depth pyramid. */
        [[nodiscard]] std::uint32_t mipCount() const noexcept { return m_mipCount; }

        /**
         * @brief Returns the dimensions of a mip level.
         * @param mip Zero-based mip-level index.
         * @return Width and height of the requested mip level.
         */
        [[nodiscard]] VkExtent2D mipExtent(std::uint32_t mip) const;

    private:
        /**
         * @brief Calculates the number of mip levels for an image extent.
         * @param width Base-level image width.
         * @param height Base-level image height.
         * @return Number of mip levels required to reduce the extent to 1x1.
         */
        static std::uint32_t calculateMipCount(
            std::uint32_t width,
            std::uint32_t height
        );

        /**
         * @brief Finds a physical-device memory type matching the requirements.
         * @param physicalDevice Physical device whose memory properties are queried.
         * @param typeFilter Candidate memory-type bit mask.
         * @param properties Required memory properties.
         * @return Compatible memory-type index.
         */
        /// Creates the underlying hierarchical-Z image.
        void createImage();

        /// Creates the full-image view and one view per mip level.
        void createViews();

        /// Creates the sampler used to read the Hi-Z pyramid.
        void createSampler();

        VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
        VkDevice m_device{VK_NULL_HANDLE};

        VkImage m_image{VK_NULL_HANDLE};
        VmaAllocation m_allocation{VK_NULL_HANDLE};
        VmaAllocator m_allocator{VK_NULL_HANDLE};
        VkImageView m_fullView{VK_NULL_HANDLE};
        VkSampler m_sampler{VK_NULL_HANDLE};

        std::vector<VkImageView> m_mipViews;

        std::uint32_t m_width{0};
        std::uint32_t m_height{0};
        std::uint32_t m_mipCount{0};
    };
    ;
}
