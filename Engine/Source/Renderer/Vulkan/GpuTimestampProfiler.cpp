#include "Engine/Renderer/Vulkan/GpuTimestampProfiler.h"

#include <array>
#include <stdexcept>

namespace Engine {
void GpuTimestampProfiler::create(const VkPhysicalDevice physicalDevice, const VkDevice device) {
    destroy();
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &properties);
    timestampPeriodNs_ = properties.limits.timestampPeriod;
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = FramesInFlight * QueriesPerFrame;
    if (vkCreateQueryPool(device, &info, nullptr, &queryPool_) != VK_SUCCESS)
        throw std::runtime_error("Could not create GPU timestamp query pool");
    device_ = device;
    submitted_.fill(false);
}

void GpuTimestampProfiler::destroy() noexcept {
    if (device_ != VK_NULL_HANDLE && queryPool_ != VK_NULL_HANDLE)
        vkDestroyQueryPool(device_, queryPool_, nullptr);
    device_ = VK_NULL_HANDLE;
    queryPool_ = VK_NULL_HANDLE;
    timestampPeriodNs_ = 0.0F;
    submitted_.fill(false);
}

void GpuTimestampProfiler::beginFrame(const VkCommandBuffer commandBuffer, const std::uint32_t frameIndex) const {
    if (queryPool_ != VK_NULL_HANDLE)
        vkCmdResetQueryPool(commandBuffer, queryPool_, frameIndex * QueriesPerFrame, QueriesPerFrame);
}

void GpuTimestampProfiler::markSubmitted(const std::uint32_t frameIndex) noexcept {
    submitted_[frameIndex] = true;
}

void GpuTimestampProfiler::beginPass(const VkCommandBuffer commandBuffer, const std::uint32_t frameIndex,
                                     const GpuProfilePass pass) const {
    if (queryPool_ != VK_NULL_HANDLE)
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queryPool_, query(frameIndex, pass, false));
}

void GpuTimestampProfiler::endPass(const VkCommandBuffer commandBuffer, const std::uint32_t frameIndex,
                                   const GpuProfilePass pass) const {
    if (queryPool_ != VK_NULL_HANDLE)
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queryPool_, query(frameIndex, pass, true));
}

std::optional<GpuProfileFrame> GpuTimestampProfiler::completedFrame(const std::uint32_t frameIndex) const {
    if (queryPool_ == VK_NULL_HANDLE || !submitted_[frameIndex]) return std::nullopt;
    std::array<std::uint64_t, QueriesPerFrame> values{};
    if (vkGetQueryPoolResults(device_, queryPool_, frameIndex * QueriesPerFrame, QueriesPerFrame,
                              sizeof(values), values.data(), sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return std::nullopt;
    GpuProfileFrame frame{};
    for (std::uint32_t pass = 0; pass < PassCount; ++pass) {
        const std::uint64_t begin = values[pass * 2];
        const std::uint64_t end = values[pass * 2 + 1];
        frame.milliseconds[pass] = end >= begin ?
            static_cast<float>(end - begin) * timestampPeriodNs_ * 1.0e-6F : 0.0F;
    }
    return frame;
}
} // namespace Engine
