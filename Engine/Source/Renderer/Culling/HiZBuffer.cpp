#include "Engine/Renderer/Culling/HiZBuffer.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace Engine::Culling {
    HiZBuffer::HiZBuffer(
        const VkPhysicalDevice physicalDevice,
        const VkDevice device,
        const std::uint32_t width,
        const std::uint32_t height
        ) {
        create(physicalDevice, device, width, height);
    }

    HiZBuffer::~HiZBuffer() {
        destroy();
    }

    HiZBuffer::HiZBuffer(HiZBuffer &&other) noexcept {
        *this = std::move(other);
    }

    HiZBuffer& HiZBuffer::operator=(HiZBuffer &&other) noexcept {
        if (this == &other) { return *this;}
        destroy();
        m_physicalDevice = other.m_physicalDevice;
        m_device = other.m_device;
        m_image = other.m_image;
        m_memory = other.m_memory;
        m_fullView = other.m_fullView;
        m_sampler = other.m_sampler;
        m_mipViews = std::move(other.m_mipViews);
        m_width = other.m_width;
        m_height = other.m_height;
        m_mipCount = other.m_mipCount;

        other.m_physicalDevice = VK_NULL_HANDLE;
        other.m_device = VK_NULL_HANDLE;
        other.m_image = VK_NULL_HANDLE;
        other.m_memory = VK_NULL_HANDLE;
        other.m_fullView = VK_NULL_HANDLE;
        other.m_sampler = VK_NULL_HANDLE;
        other.m_width = 0;
        other.m_height = 0;
        other.m_mipCount = 0;

        return *this;
    }

    void HiZBuffer::create(
        const VkPhysicalDevice physicalDevice,
        const VkDevice device,
        const std::uint32_t width,
        const std::uint32_t height
        ) {
        destroy();

        if (physicalDevice == VK_NULL_HANDLE || device == VK_NULL_HANDLE)
        {
            throw std::invalid_argument("HiZBuffer received invalid Vulkan device");
        }

        if (width == 0 || height == 0)
        {
            throw std::invalid_argument("HiZBuffer extent cannot be zero");
        }

        m_physicalDevice = physicalDevice;
        m_device = device;
        m_width = width;
        m_height = height;
        m_mipCount = calculateMipCount(width, height);

        createImage();
        createViews();
        createSampler();
    }

    void HiZBuffer::destroy()
    {
        if (m_device == VK_NULL_HANDLE)
        {
            return;
        }

        if (m_sampler != VK_NULL_HANDLE)
        {
            vkDestroySampler(m_device, m_sampler, nullptr);
            m_sampler = VK_NULL_HANDLE;
        }

        for (const VkImageView view : m_mipViews)
        {
            if (view != VK_NULL_HANDLE)
            {
                vkDestroyImageView(m_device, view, nullptr);
            }
        }

        m_mipViews.clear();

        if (m_fullView != VK_NULL_HANDLE)
        {
            vkDestroyImageView(m_device, m_fullView, nullptr);
            m_fullView = VK_NULL_HANDLE;
        }

        if (m_image != VK_NULL_HANDLE)
        {
            vkDestroyImage(m_device, m_image, nullptr);
            m_image = VK_NULL_HANDLE;
        }

        if (m_memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_device, m_memory, nullptr);
            m_memory = VK_NULL_HANDLE;
        }

        m_device = VK_NULL_HANDLE;
        m_physicalDevice = VK_NULL_HANDLE;
        m_width = 0;
        m_height = 0;
        m_mipCount = 0;
    }

    VkImageView HiZBuffer::mipView(const std::uint32_t mip) const
    {
        if (mip >= m_mipViews.size())
        {
            throw std::out_of_range("Invalid Hi-Z mip level");
        }

        return m_mipViews[mip];
    }

    VkExtent2D HiZBuffer::mipExtent(const std::uint32_t mip) const
    {
        if (mip >= m_mipCount)
        {
            throw std::out_of_range("Invalid Hi-Z mip level");
        }

        return {
            std::max(1u, m_width >> mip),
            std::max(1u, m_height >> mip)
        };
    }

    std::uint32_t HiZBuffer::calculateMipCount(
        const std::uint32_t width,
        const std::uint32_t height
    )
    {
        const std::uint32_t largest = std::max(width, height);

        return static_cast<std::uint32_t>(
            std::floor(std::log2(static_cast<double>(largest)))
        ) + 1u;
    }

    std::uint32_t HiZBuffer::findMemoryType(
        const VkPhysicalDevice physicalDevice,
        const std::uint32_t typeFilter,
        const VkMemoryPropertyFlags properties
    )
    {
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(
            physicalDevice,
            &memoryProperties
        );

        for (std::uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
        {
            const bool supported = (typeFilter & (1u << i)) != 0;
            const bool hasProperties =
                (memoryProperties.memoryTypes[i].propertyFlags & properties) ==
                properties;

            if (supported && hasProperties)
            {
                return i;
            }
        }

        throw std::runtime_error("No suitable Vulkan memory type for Hi-Z image");
    }

    void HiZBuffer::createImage()
    {
        VkImageCreateInfo imageInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R32_SFLOAT,
            .extent = {
                .width = m_width,
                .height = m_height,
                .depth = 1
            },
            .mipLevels = m_mipCount,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage =
                VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_STORAGE_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
        };

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_image) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create Hi-Z image");
        }

        VkMemoryRequirements memoryRequirements{};
        vkGetImageMemoryRequirements(
            m_device,
            m_image,
            &memoryRequirements
        );

        VkMemoryAllocateInfo allocationInfo{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = memoryRequirements.size,
            .memoryTypeIndex = findMemoryType(
                m_physicalDevice,
                memoryRequirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
            )
        };

        if (vkAllocateMemory(
                m_device,
                &allocationInfo,
                nullptr,
                &m_memory
            ) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to allocate Hi-Z image memory");
        }

        if (vkBindImageMemory(
                m_device,
                m_image,
                m_memory,
                0
            ) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to bind Hi-Z image memory");
        }
    }

    void HiZBuffer::createViews()
    {
        VkImageViewCreateInfo fullViewInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = m_image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_R32_SFLOAT,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = m_mipCount,
                .baseArrayLayer = 0,
                .layerCount = 1
            }
        };

        if (vkCreateImageView(
                m_device,
                &fullViewInfo,
                nullptr,
                &m_fullView
            ) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create full Hi-Z image view");
        }

        m_mipViews.resize(m_mipCount);

        for (std::uint32_t mip = 0; mip < m_mipCount; ++mip)
        {
            VkImageViewCreateInfo mipViewInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = m_image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = VK_FORMAT_R32_SFLOAT,
                .subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = mip,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1
                }
            };

            if (vkCreateImageView(
                    m_device,
                    &mipViewInfo,
                    nullptr,
                    &m_mipViews[mip]
                ) != VK_SUCCESS)
            {
                throw std::runtime_error("Failed to create Hi-Z mip view");
            }
        }
    }

    void HiZBuffer::createSampler()
    {
        VkSamplerCreateInfo samplerInfo{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST,
            .minFilter = VK_FILTER_NEAREST,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .mipLodBias = 0.0f,
            .anisotropyEnable = VK_FALSE,
            .maxAnisotropy = 1.0f,
            .compareEnable = VK_FALSE,
            .compareOp = VK_COMPARE_OP_ALWAYS,
            .minLod = 0.0f,
            .maxLod = static_cast<float>(m_mipCount - 1),
            .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
            .unnormalizedCoordinates = VK_FALSE
        };

        if (vkCreateSampler(
                m_device,
                &samplerInfo,
                nullptr,
                &m_sampler
            ) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create Hi-Z sampler");
        }
    }
}
