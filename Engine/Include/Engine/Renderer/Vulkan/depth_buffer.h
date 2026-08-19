#pragma once

/**
 * @file depth_buffer(1).h
 * @brief Declares the Vulkan depth-stencil buffer wrapper.
 */

#include <vulkan/vulkan.h>

#include <cstdint>

namespace Engine {

/**
 * @brief Owns the depth image, image view and sampler used by a render pass.
 *
 * The supported depth format is selected from the physical device according
 * to the requested multisampling configuration.
 */
class DepthBuffer final {
public:
    /// Creates an empty depth-buffer wrapper.
    DepthBuffer() = default;

    /// Releases the owned Vulkan resources.
    ~DepthBuffer();

    /// Copy construction is disabled because the object owns Vulkan handles.
    DepthBuffer(const DepthBuffer&) = delete;

    /// Copy assignment is disabled because the object owns Vulkan handles.
    DepthBuffer& operator=(const DepthBuffer&) = delete;

    /// Move construction is disabled because Vulkan ownership is not transferable.
    DepthBuffer(DepthBuffer&&) = delete;

    /// Move assignment is disabled because Vulkan ownership is not transferable.
    DepthBuffer& operator=(DepthBuffer&&) = delete;

    /**
     * @brief Stores the Vulkan devices required for resource creation.
     * @param physicalDevice Physical device used for format and memory queries.
     * @param device Logical device used to create and destroy resources.
     */
    void initialize(VkPhysicalDevice physicalDevice, VkDevice device);

    /**
     * @brief Creates or recreates the depth resources for a render target.
     * @param extent Width and height of the depth image.
     * @param samples Multisample count required by the render target.
     */
    void create(VkExtent2D extent, VkSampleCountFlagBits samples,
                VkFormat requiredFormat = VK_FORMAT_UNDEFINED);

    /// Releases the depth image, view, sampler and allocated memory.
    void destroy() noexcept;

    /**
     * @brief Returns the depth image handle.
     * @return The owned image handle, or VK_NULL_HANDLE if not created.
     */
    [[nodiscard]] VkImage image() const noexcept {
        return image_;
    }

    /**
     * @brief Returns the depth image-view handle.
     * @return The owned image-view handle, or VK_NULL_HANDLE if not created.
     */
    [[nodiscard]] VkImageView imageView() const noexcept {
        return imageView_;
    }

    /**
     * @brief Returns the selected depth format.
     * @return The Vulkan format, or VK_FORMAT_UNDEFINED before creation.
     */
    [[nodiscard]] VkFormat format() const noexcept {
        return format_;
    }

    /**
     * @brief Returns the depth sampler handle.
     * @return The owned sampler handle, or VK_NULL_HANDLE if not created.
     */
    [[nodiscard]] VkSampler sampler() const noexcept { return sampler_; }

private:
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;

    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView imageView_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;

    /**
     * @brief Finds a compatible physical-device memory type.
     * @param typeFilter Bit mask of candidate memory types.
     * @param requiredProperties Required Vulkan memory property flags.
     * @return Index of a compatible memory type.
     */
    [[nodiscard]] uint32_t findMemoryType(
        uint32_t typeFilter,
        VkMemoryPropertyFlags requiredProperties) const;

    /**
     * @brief Selects a supported depth format for the requested sample count.
     * @param samples Multisample count required by the depth image.
     * @return A supported Vulkan depth format.
     */
    [[nodiscard]] VkFormat findSupportedFormat(
        VkSampleCountFlagBits samples) const;
    /**
     * @brief Checks whether a format contains a stencil component.
     * @param format Vulkan format to inspect.
     * @return True when the format contains stencil data.
     */
    [[nodiscard]] static bool hasStencilComponent(VkFormat format) noexcept;
};

} // namespace Engine
