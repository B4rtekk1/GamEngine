#include "Engine/Renderer/Vulkan/vulkan_device.h"

#include <array>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <stdexcept>
#include <vector>

namespace Engine {

namespace {
    constexpr std::size_t kRequiredDeviceExtensionCount = 1;
    constexpr std::array<const char*, kRequiredDeviceExtensionCount> kRequiredDeviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };

    constexpr int kDiscreteGpuScoreBonus = 10'000;
    constexpr int kIntegratedGpuScoreBonus = 1'000;
    constexpr float kQueuePriority = 1.0F;
    constexpr uint32_t kQueueCount = 1;
    constexpr uint32_t kFirstQueueIndex = 0;
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

        VmaAllocatorCreateInfo allocatorInfo{};
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
        allocatorInfo.physicalDevice = physicalDevice_;
        allocatorInfo.device = device_;
        allocatorInfo.instance = instance;
        if (vmaCreateAllocator(&allocatorInfo, &allocator_) != VK_SUCCESS) {
            throw std::runtime_error("Could not create VMA allocator");
        }
    } catch(...) {
        destroy();
        throw;
    }
}

void VulkanDevice::destroy() noexcept {
    if (allocator_ != VK_NULL_HANDLE) {
        vmaDestroyAllocator(allocator_);
        allocator_ = VK_NULL_HANDLE;
    }
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
        if (families[i].queueCount > 0 &&
            (families[i].queueFlags & static_cast<VkQueueFlags>(VK_QUEUE_GRAPHICS_BIT)) != 0) {
            indices.graphics = i;
        }

        VkBool32 supportsPresent = VK_FALSE;
        if (const VkResult result = vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface_, &supportsPresent); result == VK_SUCCESS && supportsPresent == VK_TRUE) {
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
        for (const auto&[extensionName, specVersion] : available) {
            if (std::strcmp(extensionName, requiredExtension) == 0) {
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
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(candidate, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_3) {
        return false;
    }
    VkPhysicalDeviceVulkan13Features features13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features features12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features features11{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 features2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features2.pNext = &features11;
    features11.pNext = &features12;
    features12.pNext = &features13;
    vkGetPhysicalDeviceFeatures2(candidate, &features2);
    if (features13.synchronization2 != VK_TRUE ||
        features12.drawIndirectCount != VK_TRUE ||
        features12.shaderFloat16 != VK_TRUE ||
        features11.shaderDrawParameters != VK_TRUE ||
        features2.features.multiDrawIndirect != VK_TRUE ||
        features2.features.shaderInt16 != VK_TRUE ||
        features2.features.shaderSampledImageArrayDynamicIndexing != VK_TRUE) {
        return false;
    }
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

int VulkanDevice::scoreDevice(const VkPhysicalDevice candidate) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(candidate, &properties);

    int score = 0;

    if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
        score += kDiscreteGpuScoreBonus;
    } else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
        score += kIntegratedGpuScoreBonus;
    }

    score += static_cast<int>(properties.limits.maxImageDimension2D);
    return score;
}

void VulkanDevice::selectPhysicalDevice(const VkInstance candidate) {
    uint32_t deviceCount = 0;
    if (vkEnumeratePhysicalDevices(candidate, &deviceCount, nullptr) != VK_SUCCESS ||
        deviceCount == 0) {
        throw std::runtime_error("GPU does not support Vulkan");
    }

    std::vector<VkPhysicalDevice> candidates(deviceCount);
    if (vkEnumeratePhysicalDevices(
            candidate,
            &deviceCount,
            candidates.data()) != VK_SUCCESS) {
        throw std::runtime_error("GPU does not support Vulkan");
    }

    int bestScore = std::numeric_limits<int>::min();
    VkPhysicalDevice bestDevice = VK_NULL_HANDLE;

    for (const VkPhysicalDevice device : candidates) {
        if (!isSuitable(device)) {
            continue;
        }

        if (const int score = scoreDevice(device); score > bestScore) {
            bestScore = score;
            bestDevice = device;
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
        queueFamilies_.present.value(),
    };

    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    queueCreateInfos.reserve(uniqueFamilies.size());

    for (uint32_t family : uniqueFamilies) {
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = kQueueCount;
        queueInfo.pQueuePriorities = &kQueuePriority;
        queueCreateInfos.push_back(queueInfo);
    }

    VkPhysicalDeviceVulkan13Features features13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features features12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    features12.drawIndirectCount = VK_TRUE;
    features12.shaderFloat16 = VK_TRUE;
    features12.pNext = &features13;
    VkPhysicalDeviceVulkan11Features features11{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    features11.shaderDrawParameters = VK_TRUE;
    features11.pNext = &features12;
    VkPhysicalDeviceFeatures2 features2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features2.features.multiDrawIndirect = VK_TRUE;
    features2.features.shaderInt16 = VK_TRUE;
    features2.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    features2.pNext = &features11;

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount =
        static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pNext = &features2;
    createInfo.enabledExtensionCount =
        static_cast<uint32_t>(kRequiredDeviceExtensions.size());
    createInfo.ppEnabledExtensionNames = kRequiredDeviceExtensions.data();

    const VkResult result = vkCreateDevice(
            physicalDevice_,
            &createInfo,
            nullptr,
            &device_);
    if (result != VK_SUCCESS) {
        throw std::runtime_error(
            "GPU does not support the requested logical device creation (VkResult " +
            std::to_string(static_cast<int>(result)) + ")");
    }

    vkGetDeviceQueue(
        device_,
        queueFamilies_.graphics.value(),
        kFirstQueueIndex,
        &graphicsQueue_);

    vkGetDeviceQueue(
        device_,
        queueFamilies_.present.value(),
        kFirstQueueIndex,
        &presentQueue_);
}

} // namespace Engine
