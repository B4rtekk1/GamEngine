#include "swapchain.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
    struct SwapchainSupport {
        VkSurfaceCapabilitiesKHR capabilities{};
        std::vector<VkSurfaceFormatKHR> formats;
        std::vector<VkPresentModeKHR> presentModes;
    };

    SwapchainSupport querySupport(
        VkPhysicalDevice physicalDevice,
        VkSurfaceKHR surface) {
        SwapchainSupport support;

        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
                physicalDevice,
                surface,
                &support.capabilities) != VK_SUCCESS) {
            throw std::runtime_error("GPU does not support the requested surface capabilities");
        }

        uint32_t formatCount = 0;
        if (vkGetPhysicalDeviceSurfaceFormatsKHR(
                physicalDevice,
                surface,
                &formatCount,
                nullptr) != VK_SUCCESS || formatCount == 0) {
            throw std::runtime_error("Surface does not share any format");
        }

        support.formats.resize(formatCount);
        if (vkGetPhysicalDeviceSurfaceFormatsKHR(
                physicalDevice,
                surface,
                &formatCount,
                support.formats.data()) != VK_SUCCESS) {
            throw std::runtime_error("GPU does not support the requested surface format");
        }

        uint32_t presentModeCount = 0;
        if (vkGetPhysicalDeviceSurfacePresentModesKHR(
                physicalDevice,
                surface,
                &presentModeCount,
                nullptr) != VK_SUCCESS || presentModeCount == 0) {
            throw std::runtime_error("Surface does not share any present modes");
        }

        support.presentModes.resize(presentModeCount);
        if (vkGetPhysicalDeviceSurfacePresentModesKHR(
                physicalDevice,
                surface,
                &presentModeCount,
                support.presentModes.data()) != VK_SUCCESS) {
            throw std::runtime_error("GPU does not support the requested present modes");
        }

        return support;
    }

    VkSurfaceFormatKHR chooseSurfaceFormat(
        const std::vector<VkSurfaceFormatKHR> &formats) {
        for (const VkSurfaceFormatKHR &format: formats) {
            if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
                format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return format;
            }
        }

        return formats.front();
    }

    VkPresentModeKHR choosePresentMode(
        const std::vector<VkPresentModeKHR> &presentModes) {
        constexpr std::array preferredModes = {
            VK_PRESENT_MODE_IMMEDIATE_KHR,
            VK_PRESENT_MODE_MAILBOX_KHR,
            VK_PRESENT_MODE_FIFO_KHR
        };

        for (VkPresentModeKHR preferred: preferredModes) {
            if (std::ranges::find(presentModes
                                  ,
                                  preferred) != presentModes.end()) {
                return preferred;
            }
        }

        return VK_PRESENT_MODE_FIFO_KHR;
    }

    VkExtent2D chooseExtent(
        SDL_Window *window,
        const VkSurfaceCapabilitiesKHR &capabilities) {
        if (capabilities.currentExtent.width != UINT32_MAX) {
            return capabilities.currentExtent;
        }

        int width = 0;
        int height = 0;
        if (!SDL_GetWindowSizeInPixels(window, &width, &height)) {
            throw std::runtime_error("GPU does not support the requested window size");
        }

        VkExtent2D extent{
            static_cast<uint32_t>(std::max(width, 0)),
            static_cast<uint32_t>(std::max(height, 0))
        };

        extent.width = std::clamp(
            extent.width,
            capabilities.minImageExtent.width,
            capabilities.maxImageExtent.width);
        extent.height = std::clamp(
            extent.height,
            capabilities.minImageExtent.height,
            capabilities.maxImageExtent.height);

        return extent;
    }

    VkCompositeAlphaFlagBitsKHR chooseCompositeAlpha(
        VkCompositeAlphaFlagsKHR supported) {
        constexpr std::array candidates = {
            VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
        };

        for (VkCompositeAlphaFlagBitsKHR candidate: candidates) {
            if ((supported & candidate) != 0) {
                return candidate;
            }
        }

        throw std::runtime_error("Surface does not share composite alpha");
    }
}

Swapchain::~Swapchain() {
    destroy();
}

void Swapchain::create(
    SDL_Window *window,
    VkSurfaceKHR surface,
    const VulkanDevice &device) {
    if (window == nullptr) {
        throw std::invalid_argument("SDL_Window does not exist");
    }
    if (surface == VK_NULL_HANDLE) {
        throw std::invalid_argument("VkSurfaceKHR does not exist");
    }
    if (device.logical() == VK_NULL_HANDLE ||
        device.physical() == VK_NULL_HANDLE) {
        throw std::invalid_argument("VulkanDevice does not exist");
    }
    if (swapchain_ != VK_NULL_HANDLE) {
        throw std::logic_error("Swapchain already exists");
    }

    window_ = window;
    surface_ = surface;
    device_ = &device;

    try {
        createResources(VK_NULL_HANDLE);
    } catch (...) {
        window_ = nullptr;
        surface_ = VK_NULL_HANDLE;
        device_ = nullptr;
        throw;
    }
}

void Swapchain::recreate() {
    if (swapchain_ == VK_NULL_HANDLE || device_ == nullptr) {
        throw std::logic_error("Cannot recreate an empty swapchain");
    }

    createResources(swapchain_);
}

void Swapchain::createResources(VkSwapchainKHR oldSwapchain) {
    const SwapchainSupport support = querySupport(
        device_->physical(),
        surface_);

    const VkSurfaceFormatKHR surfaceFormat =
            chooseSurfaceFormat(support.formats);
    const VkPresentModeKHR presentMode =
            choosePresentMode(support.presentModes);
    const VkExtent2D newExtent =
            chooseExtent(window_, support.capabilities);

    uint32_t imageCount = support.capabilities.minImageCount + 1;
    if (support.capabilities.maxImageCount > 0) {
        imageCount = std::min(
            imageCount,
            support.capabilities.maxImageCount);
    }

    if ((support.capabilities.supportedUsageFlags &
         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0) {
        throw std::runtime_error(
            "Surface does not support images as color attachment");
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface_;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = newExtent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    const QueueFamilyIndices &families = device_->queueFamilies();
    const uint32_t familyIndices[] = {
        families.graphics.value(),
        families.present.value()
    };

    if (families.graphics != families.present) {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = familyIndices;
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = support.capabilities.currentTransform;
    createInfo.compositeAlpha = chooseCompositeAlpha(
        support.capabilities.supportedCompositeAlpha);
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = oldSwapchain;

    VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(
            device_->logical(),
            &createInfo,
            nullptr,
            &newSwapchain) != VK_SUCCESS) {
        throw std::runtime_error("GPU does not support the requested swapchain creation");
    }

    std::vector<VkImage> newImages;
    std::vector<VkImageView> newImageViews;

    try {
        uint32_t actualImageCount = 0;
        if (vkGetSwapchainImagesKHR(
                device_->logical(),
                newSwapchain,
                &actualImageCount,
                nullptr) != VK_SUCCESS || actualImageCount == 0) {
            throw std::runtime_error("GPU does not support the requested swapchain images");
        }

        newImages.resize(actualImageCount);
        if (vkGetSwapchainImagesKHR(
                device_->logical(),
                newSwapchain,
                &actualImageCount,
                newImages.data()) != VK_SUCCESS) {
            throw std::runtime_error("GPU does not support the requested swapchain images");
        }

        newImageViews.reserve(newImages.size());
        for (VkImage image: newImages) {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = surfaceFormat.format;
            viewInfo.components = {
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY
            };
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 1;

            VkImageView imageView = VK_NULL_HANDLE;
            if (vkCreateImageView(
                    device_->logical(),
                    &viewInfo,
                    nullptr,
                    &imageView) != VK_SUCCESS) {
                throw std::runtime_error(
                    "GPU does not support the requested image view creation for swapchain");
            }

            newImageViews.push_back(imageView);
        }
    } catch (...) {
        for (VkImageView imageView: newImageViews) {
            vkDestroyImageView(device_->logical(), imageView, nullptr);
        }
        vkDestroySwapchainKHR(device_->logical(), newSwapchain, nullptr);
        throw;
    }

    destroyImageViews();
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_->logical(), swapchain_, nullptr);
    }

    swapchain_ = newSwapchain;
    imageFormat_ = surfaceFormat.format;
    extent_ = newExtent;
    images_ = std::move(newImages);
    imageViews_ = std::move(newImageViews);
}

void Swapchain::destroyImageViews() noexcept {
    if (device_ != nullptr && device_->logical() != VK_NULL_HANDLE) {
        for (VkImageView imageView: imageViews_) {
            vkDestroyImageView(device_->logical(), imageView, nullptr);
        }
    }
    imageViews_.clear();
}

void Swapchain::destroy() noexcept {
    if (device_ != nullptr && device_->logical() != VK_NULL_HANDLE) {
        destroyImageViews();

        if (swapchain_ != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device_->logical(), swapchain_, nullptr);
        }
    } else {
        imageViews_.clear();
    }

    images_.clear();
    extent_ = {};
    imageFormat_ = VK_FORMAT_UNDEFINED;
    swapchain_ = VK_NULL_HANDLE;
    device_ = nullptr;
    surface_ = VK_NULL_HANDLE;
    window_ = nullptr;
}
