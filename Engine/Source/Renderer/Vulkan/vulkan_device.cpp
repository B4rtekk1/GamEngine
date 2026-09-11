#include "Engine/Renderer/Vulkan/vulkan_device.h"
#include "Engine/Core/Diagnostics.h"
#include "Engine/Renderer/Materials/MaterialBuffer.h"

#include <array>
#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
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
    constexpr std::array<float, 3> kQueuePriorities = {1.0F, 1.0F, 1.0F};
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
        // VMA must allocate memory with DEVICE_ADDRESS support before a
        // VkBuffer created with SHADER_DEVICE_ADDRESS_BIT can expose an address.
        allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        if (memoryBudgetExtensionSupported_) {
            allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
        }
        if (vmaCreateAllocator(&allocatorInfo, &allocator_) != VK_SUCCESS) {
            throw std::runtime_error("Could not create VMA allocator");
        }
        memoryBudgetManager_.initialize(physicalDevice_, allocator_);
    } catch(...) {
        destroy();
        throw;
    }
}

void VulkanDevice::destroy() noexcept {
    memoryBudgetManager_.reset();
    if (allocator_ != VK_NULL_HANDLE) {
        vmaDestroyAllocator(allocator_);
        allocator_ = VK_NULL_HANDLE;
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
    }

    presentQueue_ = VK_NULL_HANDLE;
    graphicsQueue_ = VK_NULL_HANDLE;
    computeQueue_ = VK_NULL_HANDLE;
    transferQueue_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    physicalDevice_ = VK_NULL_HANDLE;
    surface_ = VK_NULL_HANDLE;
    queueFamilies_ = {};
    depthResolveMode_ = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    memoryBudgetExtensionSupported_ = false;
}

QueueFamilyIndices VulkanDevice::findQueueFamilies(VkPhysicalDevice candidate) const {
    QueueFamilyIndices indices;

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueFamilyCount, nullptr);


    std::vector<VkQueueFamilyProperties> families(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueFamilyCount, families.data());

    std::optional<uint32_t> dedicatedCompute;
    std::optional<uint32_t> generalCompute;
    std::optional<uint32_t> dedicatedTransfer;
    std::optional<uint32_t> nonGraphicsTransfer;

    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        const VkQueueFamilyProperties& family = families[i];
        if (family.queueCount == 0) {
            continue;
        }

        if ((family.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && !indices.graphics.has_value()) {
            indices.graphics = i;
        }

        if ((family.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0) {
            if ((family.queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0 && !dedicatedCompute.has_value()) {
                dedicatedCompute = i;
            }
            if (!generalCompute.has_value()) {
                generalCompute = i;
            }
        }
        if ((family.queueFlags & VK_QUEUE_TRANSFER_BIT) != 0) {
            const bool hasGraphics = (family.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
            const bool hasCompute = (family.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
            if (!hasGraphics && !hasCompute && !dedicatedTransfer.has_value()) dedicatedTransfer = i;
            if (!hasGraphics && !nonGraphicsTransfer.has_value()) nonGraphicsTransfer = i;
        }

        VkBool32 supportsPresent = VK_FALSE;
        if (const VkResult result = vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface_, &supportsPresent); result == VK_SUCCESS && supportsPresent == VK_TRUE) {
            indices.present = i;
        }

    }

    if (!indices.graphics.has_value()) {
        return indices;
    }

    // Prefer a compute-only family. If none exists, request a second queue
    // from the graphics family; queue family and queue index are distinct.
    if (dedicatedCompute.has_value()) {
        indices.compute = dedicatedCompute;
        indices.dedicatedComputeFamily = true;
        indices.asyncCompute = dedicatedCompute != indices.graphics;
    } else {
        const uint32_t graphicsFamily = indices.graphics.value();
        if ((families[graphicsFamily].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0) {
            indices.compute = graphicsFamily;
            if (families[graphicsFamily].queueCount > 1) {
                indices.computeIndex = 1;
                indices.asyncCompute = true;
            }
        } else if (generalCompute.has_value()) {
            indices.compute = generalCompute;
            indices.asyncCompute = generalCompute != indices.graphics;
        }
    }

    // A Vulkan graphics queue is normally also compute-capable. Retain a
    // defined fallback for unusual devices that expose no compute queue.
    if (!indices.compute.has_value()) {
        indices.compute = indices.graphics;
    }
    if (dedicatedTransfer.has_value()) {
        indices.transfer = dedicatedTransfer;
        indices.dedicatedTransferFamily = true;
        indices.asyncTransfer = dedicatedTransfer != indices.graphics;
    } else if (nonGraphicsTransfer.has_value()) {
        indices.transfer = nonGraphicsTransfer;
        indices.asyncTransfer = nonGraphicsTransfer != indices.graphics;
    } else {
        indices.transfer = indices.graphics;
        const uint32_t graphicsFamily = indices.graphics.value();
        uint32_t nextIndex = 1;
        if (indices.compute == indices.graphics) nextIndex = std::max(nextIndex, indices.computeIndex + 1);
        if (families[graphicsFamily].queueCount > nextIndex) {
            indices.transferIndex = nextIndex;
            indices.asyncTransfer = true;
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
    if (features13.dynamicRendering != VK_TRUE ||
        features13.synchronization2 != VK_TRUE ||
        features12.timelineSemaphore != VK_TRUE ||
        features12.drawIndirectCount != VK_TRUE ||
        features12.shaderFloat16 != VK_TRUE ||
        features12.bufferDeviceAddress != VK_TRUE ||
        features11.shaderDrawParameters != VK_TRUE ||
        features2.features.multiDrawIndirect != VK_TRUE ||
        features2.features.shaderInt16 != VK_TRUE ||
        features2.features.shaderSampledImageArrayDynamicIndexing != VK_TRUE ||
        features2.features.textureCompressionBC != VK_TRUE) {
        return false;
    }
    if (features12.descriptorIndexing != VK_TRUE ||
        features12.runtimeDescriptorArray != VK_TRUE ||
        features12.descriptorBindingPartiallyBound != VK_TRUE ||
        features12.descriptorBindingVariableDescriptorCount != VK_TRUE ||
        features12.shaderSampledImageArrayNonUniformIndexing != VK_TRUE) {
        return false;
    }
    if (properties.limits.maxPerStageDescriptorSampledImages < MaxMaterialTextures) return false;
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
    uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> extensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extensionCount, extensions.data());
    memoryBudgetExtensionSupported_ = std::any_of(extensions.begin(), extensions.end(), [](const VkExtensionProperties& extension) {
        return std::strcmp(extension.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0;
    });
    queueFamilies_ = findQueueFamilies(physicalDevice_);
    VkPhysicalDeviceDepthStencilResolveProperties depthResolveProperties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES};
    VkPhysicalDeviceProperties2 properties2{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &depthResolveProperties};
    vkGetPhysicalDeviceProperties2(physicalDevice_, &properties2);
    if ((depthResolveProperties.supportedDepthResolveModes & VK_RESOLVE_MODE_MAX_BIT) != 0) {
        depthResolveMode_ = VK_RESOLVE_MODE_MAX_BIT;
    }
}

void VulkanDevice::createLogicalDevice() {
    if (physicalDevice_ == VK_NULL_HANDLE || !queueFamilies_.complete()) {
        throw std::logic_error("GPU does not exist");
    }

    std::map<uint32_t, uint32_t> requestedQueueCounts;
    const auto requireQueue = [&requestedQueueCounts](const uint32_t family, const uint32_t index) {
        auto& count = requestedQueueCounts[family];
        count = std::max(count, index + 1);
    };
    requireQueue(queueFamilies_.graphics.value(), queueFamilies_.graphicsIndex);
    requireQueue(queueFamilies_.present.value(), queueFamilies_.presentIndex);
    requireQueue(queueFamilies_.compute.value(), queueFamilies_.computeIndex);
    requireQueue(queueFamilies_.transfer.value(), queueFamilies_.transferIndex);

    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    queueCreateInfos.reserve(requestedQueueCounts.size());

    for (const auto& [family, count] : requestedQueueCounts) {
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = count;
        queueInfo.pQueuePriorities = kQueuePriorities.data();
        queueCreateInfos.push_back(queueInfo);
    }

    VkPhysicalDeviceVulkan13Features features13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features features12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    features12.timelineSemaphore = VK_TRUE;
    features12.drawIndirectCount = VK_TRUE;
    features12.shaderFloat16 = VK_TRUE;
    features12.bufferDeviceAddress = VK_TRUE;
    features12.descriptorIndexing = VK_TRUE;
    features12.runtimeDescriptorArray = VK_TRUE;
    features12.descriptorBindingPartiallyBound = VK_TRUE;
    features12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    features12.pNext = &features13;
    VkPhysicalDeviceVulkan11Features features11{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    features11.shaderDrawParameters = VK_TRUE;
    features11.pNext = &features12;
    VkPhysicalDeviceFeatures2 features2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features2.features.multiDrawIndirect = VK_TRUE;
    features2.features.shaderInt16 = VK_TRUE;
    features2.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    features2.features.textureCompressionBC = VK_TRUE;
    features2.pNext = &features11;

    std::vector<const char*> enabledExtensions(kRequiredDeviceExtensions.begin(), kRequiredDeviceExtensions.end());
    if (memoryBudgetExtensionSupported_) enabledExtensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount =
        static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pNext = &features2;
    createInfo.enabledExtensionCount =
        static_cast<uint32_t>(enabledExtensions.size());
    createInfo.ppEnabledExtensionNames = enabledExtensions.data();

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
        queueFamilies_.graphicsIndex,
        &graphicsQueue_);

    vkGetDeviceQueue(
        device_,
        queueFamilies_.present.value(),
        queueFamilies_.presentIndex,
        &presentQueue_);

    vkGetDeviceQueue(
        device_,
        queueFamilies_.compute.value(),
        queueFamilies_.computeIndex,
        &computeQueue_);
    vkGetDeviceQueue(
        device_,
        queueFamilies_.transfer.value(),
        queueFamilies_.transferIndex,
        &transferQueue_);

    Diagnostics::instance().report(
        DiagnosticSeverity::Info,
        "Vulkan queues: Graphics: family=" + std::to_string(queueFamilies_.graphics.value()) +
        " index=" + std::to_string(queueFamilies_.graphicsIndex) +
        "; Compute: family=" + std::to_string(queueFamilies_.compute.value()) +
        " index=" + std::to_string(queueFamilies_.computeIndex) +
        "; Transfer: family=" + std::to_string(queueFamilies_.transfer.value()) +
        " index=" + std::to_string(queueFamilies_.transferIndex) +
        "; Async compute: " + (queueFamilies_.asyncCompute ? "YES" : "NO") +
        "; Async transfer: " + (queueFamilies_.asyncTransfer ? "YES" : "NO") +
        "; Dedicated transfer family: " + (queueFamilies_.dedicatedTransferFamily ? "YES" : "NO") +
        "; Dedicated compute family: " + (queueFamilies_.dedicatedComputeFamily ? "YES" : "NO"),
        {.subsystem = "Vulkan"});
}

} // namespace Engine
