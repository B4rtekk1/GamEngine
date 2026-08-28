#pragma once

/**
 * @file hdr_buffer.h
 * @brief Declares the Vulkan HDR color-render target wrapper.
 */

#include <vulkan/vulkan.h>

namespace Engine {
    /**
     * @brief Owns an HDR floating-point color image and its sampling resources.
     *
     * The image uses a 16-bit floating-point RGBA format suitable for preserving
     * high-dynamic-range lighting values before tone mapping.
     */
    class HdrBuffer final {
    public:
        /// HDR color format used by the render target.
        static constexpr VkFormat Format = VK_FORMAT_R16G16B16A16_SFLOAT;

        /// Creates an empty HDR buffer wrapper.
        HdrBuffer() = default;

        /// Releases the owned Vulkan resources.
        ~HdrBuffer();

        /// Copy construction is disabled because the object owns Vulkan handles.
        HdrBuffer(const HdrBuffer &) = delete;

        /// Copy assignment is disabled because the object owns Vulkan handles.
        HdrBuffer &operator=(const HdrBuffer &) = delete;

        /**
         * @brief Creates the HDR image, view and sampler.
         * @param physicalDevice Physical device used for image-memory selection.
         * @param device Logical Vulkan device used to create the resources.
         * @param extent Width and height of the HDR render target.
         */
        void create(VkPhysicalDevice physicalDevice, VkDevice device, VkExtent2D extent);

        /// Releases the HDR image, memory, view and sampler.
        void destroy() noexcept;

        /** @brief Returns the HDR image handle. */
        [[nodiscard]] VkImage image() const noexcept { return image_; }
        /** @brief Returns the HDR image-view handle. */
        [[nodiscard]] VkImageView imageView() const noexcept { return imageView_; }
        /** @brief Returns the sampler used to sample the HDR image. */
        [[nodiscard]] VkSampler sampler() const noexcept { return sampler_; }

    private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkImage image_ = VK_NULL_HANDLE;
        VkDeviceMemory memory_ = VK_NULL_HANDLE;
        VkImageView imageView_ = VK_NULL_HANDLE;
        VkSampler sampler_ = VK_NULL_HANDLE;
    };
} // namespace Engine
