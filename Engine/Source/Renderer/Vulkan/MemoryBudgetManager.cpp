#include "Engine/Renderer/Vulkan/MemoryBudgetManager.h"

#include <array>

namespace Engine {
void MemoryBudgetManager::initialize(const VkPhysicalDevice physicalDevice, const VmaAllocator allocator) noexcept {
    physicalDevice_ = physicalDevice; allocator_ = allocator;
    if (physicalDevice_ != VK_NULL_HANDLE) vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties_);
}
void MemoryBudgetManager::reset() noexcept { physicalDevice_ = VK_NULL_HANDLE; allocator_ = VK_NULL_HANDLE; memoryProperties_ = {}; }
bool MemoryBudgetManager::available() const noexcept { return allocator_ != VK_NULL_HANDLE && physicalDevice_ != VK_NULL_HANDLE; }
GpuMemoryCategory MemoryBudgetManager::categoryForHeap(const std::uint32_t heap) const noexcept {
    VkMemoryPropertyFlags flags{};
    for (std::uint32_t type = 0; type < memoryProperties_.memoryTypeCount; ++type)
        if (memoryProperties_.memoryTypes[type].heapIndex == heap) flags |= memoryProperties_.memoryTypes[type].propertyFlags;
    if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) return GpuMemoryCategory::DeviceLocal;
    if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) return GpuMemoryCategory::HostVisible;
    return GpuMemoryCategory::Other;
}
std::vector<GpuMemoryHeapBudget> MemoryBudgetManager::heaps() const {
    if (!available()) return {};
    std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{}; vmaGetHeapBudgets(allocator_, budgets.data());
    std::vector<GpuMemoryHeapBudget> result; result.reserve(memoryProperties_.memoryHeapCount);
    for (std::uint32_t heap = 0; heap < memoryProperties_.memoryHeapCount; ++heap) {
        const auto& source = budgets[heap]; result.push_back({heap, source.budget, source.usage, source.statistics.allocationBytes, source.statistics.allocationCount, source.statistics.blockCount, categoryForHeap(heap)});
    }
    return result;
}
std::vector<GpuMemoryCategoryBudget> MemoryBudgetManager::categories() const {
    std::array<GpuMemoryCategoryBudget, 3> totals{{{GpuMemoryCategory::DeviceLocal}, {GpuMemoryCategory::HostVisible}, {GpuMemoryCategory::Other}}};
    for (const auto& heap : heaps()) { auto& total = totals[static_cast<std::size_t>(heap.category)]; total.budget += heap.budget; total.usage += heap.usage; total.allocationBytes += heap.allocationBytes; total.allocationCount += heap.allocationCount; }
    return {totals.begin(), totals.end()};
}
} // namespace Engine
