#include "vulkan_device.h"

#include <array>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>

namespace {
    constexpr std::array<const char*, 1> kRequiredDeviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME
    };
}

VulkanDevice::~VulkanDevice() { destroy();}

void VulkanDevice::create(VkInstance instance, VkSurfaceKHR surface) {
    if (instance == VK_NULL_HANDLE) {
        throw std::invalid_argument("Vulkan instance is null");
    }
    if (surface == VK_NULL_HANDLE) {
        throw std::invalid_argument("Vulkan surface is null");
    }
    if (device_ != VK_NULL_HANDLE || physicalDevice_ != VK_NULL_HANDLE) {
        throw std::logic_error("Vulkan device already created");
    }
    surface_ = surface;

    try {
        selectPhysicalDevice(instance);
        createLogicalDevice();
    } catch(...) {
        destroy();
        throw;
    }
}

void VulkanDevice::destroy() noexcept {
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
    }

    presentQueue_ = VK_NULL_HANDLE;
    graphicsQueue_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    physicalDevice_ = VK_NULL_HANDLE;
    surface_ = VK_NULL_HANDLE;
    queueFamilies_ = {};
}

QueueFamilyIndices VulkanDevice::findQueueFamilies(VkPhysicalDevice candidate) const {
    QueueFamilyIndices indices;

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueFamilyCount, nullptr);


    std::vector<VkQueueFamilyProperties> families(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueFamilyCount, families.data());

    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (families[i].queueCount > 0 && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            indices.graphics = i;
        }

        VkBool32 supportsPresent = VK_FALSE;
        const VkResult result = vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface_, &supportsPresent);
        if (result == VK_SUCCESS && supportsPresent == VK_TRUE) {
            indices.present = i;
        }

        if (indices.complete()) {
            break;
        }
    }
    return indices;
}

bool VulkanDevice::supportsRequiredExtensions(
    VkPhysicalDevice candidate
    ) {
    uint32_t extensionCount = 0;
    if (vkEnumerateDeviceExtensionProperties(
        candidate,
        nullptr,
        &extensionCount,
        nullptr) != VK_SUCCESS) {
        return false;
    }

    std::vector<VkExtensionProperties> available(extensionCount);
    if (vkEnumerateDeviceExtensionProperties(
        candidate,
        nullptr,
        &extensionCount,
        available.data()) != VK_SUCCESS) {
        return false;
    }

    for (const auto& requiredExtension : kRequiredDeviceExtensions) {
        bool found = false;
        for (const auto& availableExtension : available) {
            if (std::strcmp(availableExtension.extensionName, requiredExtension) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }

    return true;
}

bool VulkanDevice::isSuitable(VkPhysicalDevice candidate) const {
    if (!findQueueFamilies(candidate).complete()) {
        return false;
    }

    if (!supportsRequiredExtensions(candidate)) {
        return false;
    }

    return hasAdequateSwapchain(candidate);
}

bool VulkanDevice::hasAdequateSwapchain(VkPhysicalDevice candidate) const {
    uint32_t formatCount = 0;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(candidate, surface_, &formatCount, nullptr) != VK_SUCCESS ||
        formatCount == 0) {
        return false;
    }

    uint32_t presentModeCount = 0;
    return vkGetPhysicalDeviceSurfacePresentModesKHR(
               candidate, surface_, &presentModeCount, nullptr) == VK_SUCCESS &&
           presentModeCount > 0;
}

int VulkanDevice::scoreDevice(VkPhysicalDevice candidate) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(candidate, &properties);

    int score = 0;

    if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
        score += 10'000;
    } else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
        score += 1'000;
    }

    score += static_cast<int>(properties.limits.maxImageDimension2D);
    return score;
}

void VulkanDevice::selectPhysicalDevice(VkInstance instance) {
    uint32_t deviceCount = 0;
    if (vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr) != VK_SUCCESS ||
        deviceCount == 0) {
        throw std::runtime_error("GPU does not support Vulkan");
    }

    std::vector<VkPhysicalDevice> candidates(deviceCount);
    if (vkEnumeratePhysicalDevices(
            instance,
            &deviceCount,
            candidates.data()) != VK_SUCCESS) {
        throw std::runtime_error("GPU does not support Vulkan");
    }

    int bestScore = std::numeric_limits<int>::min();
    VkPhysicalDevice bestDevice = VK_NULL_HANDLE;

    for (VkPhysicalDevice candidate : candidates) {
        if (!isSuitable(candidate)) {
            continue;
        }

        const int score = scoreDevice(candidate);
        if (score > bestScore) {
            bestScore = score;
            bestDevice = candidate;
        }
    }

    if (bestDevice == VK_NULL_HANDLE) {
        throw std::runtime_error(
            "GPU does not support the required queues and swapchain");
    }

    physicalDevice_ = bestDevice;
    queueFamilies_ = findQueueFamilies(physicalDevice_);
}

void VulkanDevice::createLogicalDevice() {
    if (physicalDevice_ == VK_NULL_HANDLE || !queueFamilies_.complete()) {
        throw std::logic_error("GPU does not exist");
    }

    const std::set<uint32_t> uniqueFamilies = {
        queueFamilies_.graphics.value(),
        queueFamilies_.present.value()
    };

    constexpr float queuePriority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    queueCreateInfos.reserve(uniqueFamilies.size());

    for (uint32_t family : uniqueFamilies) {
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueInfo);
    }

    VkPhysicalDeviceFeatures enabledFeatures{};

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount =
        static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &enabledFeatures;
    createInfo.enabledExtensionCount =
        static_cast<uint32_t>(kRequiredDeviceExtensions.size());
    createInfo.ppEnabledExtensionNames = kRequiredDeviceExtensions.data();

    if (vkCreateDevice(
            physicalDevice_,
            &createInfo,
            nullptr,
            &device_) != VK_SUCCESS) {
        throw std::runtime_error("GPU does not support the requested logical device creation");
    }

    vkGetDeviceQueue(
        device_,
        queueFamilies_.graphics.value(),
        0,
        &graphicsQueue_);

    vkGetDeviceQueue(
        device_,
        queueFamilies_.present.value(),
        0,
        &presentQueue_);
}
