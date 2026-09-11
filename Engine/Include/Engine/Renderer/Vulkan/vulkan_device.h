#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include "Engine/Renderer/Vulkan/MemoryBudgetManager.h"

#include <cstdint>
#include <optional>

namespace Engine {
    struct QueueFamilyIndices {
        std::optional<uint32_t> graphics;
        std::optional<uint32_t> present;
        std::optional<uint32_t> compute;
        uint32_t graphicsIndex{};
        uint32_t presentIndex{};
        uint32_t computeIndex{};
        bool asyncCompute{};
        bool dedicatedComputeFamily{};

        [[nodiscard]] bool complete() const noexcept {
            return graphics.has_value() && present.has_value();
        }
    };

    class VulkanDevice final {
    public:
        VulkanDevice() = default;

        ~VulkanDevice();

        VulkanDevice(const VulkanDevice &) = delete;

        VulkanDevice &operator=(const VulkanDevice &) = delete;

        VulkanDevice(VulkanDevice &&) = delete;

        VulkanDevice &operator=(VulkanDevice &&) = delete;

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

        [[nodiscard]] VkQueue computeQueue() const noexcept {
            return computeQueue_;
        }

        [[nodiscard]] const QueueFamilyIndices &queueFamilies() const noexcept {
            return queueFamilies_;
        }

        [[nodiscard]] uint32_t graphicsQueueFamily() const {
            return queueFamilies_.graphics.value();
        }

        [[nodiscard]] uint32_t presentQueueFamily() const {
            return queueFamilies_.present.value();
        }

        [[nodiscard]] uint32_t computeQueueFamily() const {
            return queueFamilies_.compute.value();
        }

        [[nodiscard]] bool hasAsyncComputeQueue() const noexcept {
            return queueFamilies_.asyncCompute;
        }

        [[nodiscard]] bool hasDedicatedComputeFamily() const noexcept {
            return queueFamilies_.dedicatedComputeFamily;
        }

        [[nodiscard]] VmaAllocator allocator() const noexcept {
            return allocator_;
        }

        [[nodiscard]] VkResolveModeFlagBits depthResolveMode() const noexcept {
            return depthResolveMode_;
        }

        [[nodiscard]] bool supportsConservativeDepthResolve() const noexcept {
            return depthResolveMode_ == VK_RESOLVE_MODE_MAX_BIT;
        }

        [[nodiscard]] const MemoryBudgetManager& memoryBudgetManager() const noexcept {
            return memoryBudgetManager_;
        }

    private:
        VkSurfaceKHR surface_ = VK_NULL_HANDLE;
        VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
        VkDevice device_ = VK_NULL_HANDLE;
        VkQueue graphicsQueue_ = VK_NULL_HANDLE;
        VkQueue presentQueue_ = VK_NULL_HANDLE;
        VkQueue computeQueue_ = VK_NULL_HANDLE;
        QueueFamilyIndices queueFamilies_{};
        VmaAllocator allocator_ = VK_NULL_HANDLE;
        VkResolveModeFlagBits depthResolveMode_ = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        bool memoryBudgetExtensionSupported_ = false;
        MemoryBudgetManager memoryBudgetManager_{};

        [[nodiscard]] QueueFamilyIndices findQueueFamilies(VkPhysicalDevice candidate) const;

        [[nodiscard]] static bool supportsRequiredExtensions(VkPhysicalDevice candidate);

        [[nodiscard]] bool hasAdequateSwapchain(VkPhysicalDevice candidate) const;

        [[nodiscard]] bool isSuitable(VkPhysicalDevice candidate) const;

        [[nodiscard]] static int scoreDevice(VkPhysicalDevice candidate);

        void selectPhysicalDevice(VkInstance candidate);

        void createLogicalDevice();
    };
} // namespace Engine
