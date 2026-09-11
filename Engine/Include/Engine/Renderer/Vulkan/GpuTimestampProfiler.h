#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>
#include <vulkan/vulkan.h>

#include "Engine/Renderer/Renderer.h"

namespace Engine {
    /** Fence-safe, dynamically named Vulkan timestamp timeline collector. */
    class GpuTimestampProfiler final {
    public:
        void create(VkPhysicalDevice physicalDevice, VkDevice device);
        void destroy() noexcept;
        void beginFrame(VkCommandBuffer commandBuffer, std::uint32_t frameIndex) const;
        void endFrame(VkCommandBuffer commandBuffer, std::uint32_t frameIndex) const;
        void markSubmitted(std::uint32_t frameIndex, std::uint64_t frameNumber) noexcept;
        [[nodiscard]] bool hasCompletedFrame() const noexcept;
        void beginZone(VkCommandBuffer commandBuffer, std::uint32_t frameIndex, ProfileNameId name) const;
        void endZone(VkCommandBuffer commandBuffer, std::uint32_t frameIndex) const;
        [[nodiscard]] std::optional<GpuProfileFrame> completedFrame(std::uint32_t frameIndex) const;

    private:
        static constexpr std::uint32_t FramesInFlight = 3;
        static constexpr std::uint32_t MaxZonesPerFrame = 64;
        static constexpr std::uint32_t QueriesPerFrame = 2 + MaxZonesPerFrame * 2;
        struct PendingEvent final { ProfileNameId name{}; std::uint16_t depth{}; };
        [[nodiscard]] static constexpr std::uint32_t query(std::uint32_t frame, std::uint32_t local) noexcept {
            return frame * QueriesPerFrame + local;
        }

        VkDevice device_{VK_NULL_HANDLE};
        VkQueryPool queryPool_{VK_NULL_HANDLE};
        float timestampPeriodNs_{};
        std::array<bool, FramesInFlight> submitted_{};
        mutable std::array<std::vector<PendingEvent>, FramesInFlight> events_;
        mutable std::array<std::vector<std::uint32_t>, FramesInFlight> zoneStack_;
        mutable std::array<std::uint32_t, FramesInFlight> suppressedZoneDepth_{};
        std::array<std::uint64_t, FramesInFlight> frameNumbers_{};
        mutable bool hasCompletedFrame_{};
    };
} // namespace Engine
