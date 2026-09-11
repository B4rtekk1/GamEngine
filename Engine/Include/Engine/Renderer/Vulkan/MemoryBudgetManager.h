#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Engine {
    enum class GpuMemoryCategory : std::uint8_t { DeviceLocal, HostVisible, Other };

    struct GpuMemoryHeapBudget final {
        std::uint32_t heapIndex{};
        VkDeviceSize budget{};
        VkDeviceSize usage{};
        VkDeviceSize allocationBytes{};
        std::uint32_t allocationCount{};
        std::uint32_t blockCount{};
        GpuMemoryCategory category{GpuMemoryCategory::Other};
    };
    struct GpuMemoryCategoryBudget final {
        GpuMemoryCategory category{GpuMemoryCategory::Other};
        VkDeviceSize budget{};
        VkDeviceSize usage{};
        VkDeviceSize allocationBytes{};
        std::uint32_t allocationCount{};
    };
    /** Actual VMA allocations used by the current GPU Scene snapshot. */
    struct GpuSceneMemoryAllocation final {
        std::string table;
        std::uint32_t heapIndex{};
        bool deviceLocal{};
        bool hostVisible{};
        bool hostCoherent{};
        VkDeviceSize bytes{};
    };

    /** Live VMA/VK_EXT_memory_budget snapshotter for engine diagnostics. */
    class MemoryBudgetManager final {
    public:
        void initialize(VkPhysicalDevice physicalDevice, VmaAllocator allocator) noexcept;
        void reset() noexcept;
        [[nodiscard]] bool available() const noexcept;
        [[nodiscard]] std::vector<GpuMemoryHeapBudget> heaps() const;
        [[nodiscard]] std::vector<GpuMemoryCategoryBudget> categories() const;
    private:
        VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
        VmaAllocator allocator_{VK_NULL_HANDLE};
        VkPhysicalDeviceMemoryProperties memoryProperties_{};
        [[nodiscard]] GpuMemoryCategory categoryForHeap(std::uint32_t heapIndex) const noexcept;
    };
} // namespace Engine
