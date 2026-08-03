#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <optional>

struct QueueFamilyIndices {
    std::optional<uint32_t> graphics;
    std::optional<uint32_t> present;

    [[nodiscard]] bool complete() const noexcept {
        return graphics.has_value() && present.has_value();
    }
};

class VulkanDevice final {
public:
    VulkanDevice() = default;
    ~VulkanDevice();

    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;
    VulkanDevice(VulkanDevice&&) = delete;
    VulkanDevice& operator=(VulkanDevice&&) = delete;

    void create(VkInstance instance, VkSurfaceKHR surface);
    void destroy() noexcept;

    [[nodiscard]] VkPhysicalDevice physical() const noexcept {
        return physicalDevice_;
    }

    [[nodiscard]] VkDevice logical() const noexcept {
        return device_;
    }

    [[nodiscard]] VkQueue graphicsQueue() const noexcept {
        return graphicsQueue_;
    }

    [[nodiscard]] VkQueue presentQueue() const noexcept {
        return presentQueue_;
    }

    [[nodiscard]] const QueueFamilyIndices& queueFamilies() const noexcept {
        return queueFamilies_;
    }

    [[nodiscard]] uint32_t graphicsQueueFamily() const {
        return queueFamilies_.graphics.value();
    }

    [[nodiscard]] uint32_t presentQueueFamily() const {
        return queueFamilies_.present.value();
    }

private:
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;
    QueueFamilyIndices queueFamilies_{};

    [[nodiscard]] QueueFamilyIndices findQueueFamilies(VkPhysicalDevice candidate) const;
    [[nodiscard]] static bool supportsRequiredExtensions(VkPhysicalDevice candidate) ;
    [[nodiscard]] bool hasAdequateSwapchain(VkPhysicalDevice candidate) const;
    [[nodiscard]] bool isSuitable(VkPhysicalDevice candidate) const;
    [[nodiscard]] static int scoreDevice(VkPhysicalDevice candidate) ;

    void selectPhysicalDevice(VkInstance candidate);
    void createLogicalDevice();
};