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
    for (auto& events : events_) events.reserve(MaxZonesPerFrame);
    for (auto& stack : zoneStack_) stack.reserve(MaxZonesPerFrame);
    suppressedZoneDepth_.fill(0);
    frameNumbers_.fill(0);
    hasCompletedFrame_ = false;
}

void GpuTimestampProfiler::destroy() noexcept {
    if (device_ != VK_NULL_HANDLE && queryPool_ != VK_NULL_HANDLE)
        vkDestroyQueryPool(device_, queryPool_, nullptr);
    device_ = VK_NULL_HANDLE;
    queryPool_ = VK_NULL_HANDLE;
    timestampPeriodNs_ = 0.0F;
    submitted_.fill(false);
    for (auto& events : events_) events.clear();
    for (auto& stack : zoneStack_) stack.clear();
    suppressedZoneDepth_.fill(0);
    frameNumbers_.fill(0);
    hasCompletedFrame_ = false;
}

void GpuTimestampProfiler::beginFrame(const VkCommandBuffer commandBuffer, const std::uint32_t frameIndex) const {
    if (queryPool_ == VK_NULL_HANDLE) return;
    vkCmdResetQueryPool(commandBuffer, queryPool_, query(frameIndex, 0), QueriesPerFrame);
    events_[frameIndex].clear();
    zoneStack_[frameIndex].clear();
    suppressedZoneDepth_[frameIndex] = 0;
    vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queryPool_, query(frameIndex, 0));
}

void GpuTimestampProfiler::endFrame(const VkCommandBuffer commandBuffer, const std::uint32_t frameIndex) const {
    if (queryPool_ == VK_NULL_HANDLE) return;
    while (!zoneStack_[frameIndex].empty()) endZone(commandBuffer, frameIndex);
    vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queryPool_, query(frameIndex, 1));
}

void GpuTimestampProfiler::markSubmitted(const std::uint32_t frameIndex, const std::uint64_t frameNumber) noexcept {
    submitted_[frameIndex] = true;
    frameNumbers_[frameIndex] = frameNumber;
}

bool GpuTimestampProfiler::hasCompletedFrame() const noexcept {
    return hasCompletedFrame_;
}

void GpuTimestampProfiler::beginZone(const VkCommandBuffer commandBuffer, const std::uint32_t frameIndex,
                                     const ProfileNameId name) const {
    if (queryPool_ == VK_NULL_HANDLE) return;
    if (suppressedZoneDepth_[frameIndex] != 0 || events_[frameIndex].size() == MaxZonesPerFrame) {
        ++suppressedZoneDepth_[frameIndex];
        return;
    }
    const std::uint32_t eventIndex = static_cast<std::uint32_t>(events_[frameIndex].size());
    events_[frameIndex].push_back({name, static_cast<std::uint16_t>(zoneStack_[frameIndex].size())});
    zoneStack_[frameIndex].push_back(eventIndex);
    vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queryPool_, query(frameIndex, 2 + eventIndex * 2));
}

void GpuTimestampProfiler::endZone(const VkCommandBuffer commandBuffer, const std::uint32_t frameIndex) const {
    if (queryPool_ == VK_NULL_HANDLE) return;
    if (suppressedZoneDepth_[frameIndex] != 0) {
        --suppressedZoneDepth_[frameIndex];
        return;
    }
    if (zoneStack_[frameIndex].empty()) return;
    const std::uint32_t eventIndex = zoneStack_[frameIndex].back();
    zoneStack_[frameIndex].pop_back();
    vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queryPool_, query(frameIndex, 3 + eventIndex * 2));
}

std::optional<GpuProfileFrame> GpuTimestampProfiler::completedFrame(const std::uint32_t frameIndex) const {
    if (queryPool_ == VK_NULL_HANDLE || !submitted_[frameIndex]) return std::nullopt;
    const std::uint32_t queryCount = 2 + static_cast<std::uint32_t>(events_[frameIndex].size()) * 2;
    std::array<std::uint64_t, QueriesPerFrame> values{};
    if (vkGetQueryPoolResults(device_, queryPool_, query(frameIndex, 0), queryCount,
                              sizeof(values), values.data(), sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return std::nullopt;
    const std::uint64_t frameBegin = values[0];
    const auto milliseconds = [this, frameBegin](const std::uint64_t value) {
        return value >= frameBegin ? static_cast<float>(value - frameBegin) * timestampPeriodNs_ * 1.0e-6F : 0.0F;
    };
    GpuProfileFrame frame{};
    frame.frameNumber = frameNumbers_[frameIndex];
    frame.frameMilliseconds = milliseconds(values[1]);
    frame.events.reserve(events_[frameIndex].size());
    for (std::uint32_t index = 0; index < events_[frameIndex].size(); ++index) {
        const PendingEvent& event = events_[frameIndex][index];
        frame.events.push_back({event.name, milliseconds(values[2 + index * 2]),
                                milliseconds(values[3 + index * 2]), event.depth});
    }
    hasCompletedFrame_ = true;
    return frame;
}
} // namespace Engine
