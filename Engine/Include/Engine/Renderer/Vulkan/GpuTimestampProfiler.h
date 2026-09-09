#pragma once

#include <cstdint>
#include <optional>
#include <vulkan/vulkan.h>

#include "Engine/Renderer/Renderer.h"

namespace Engine {
    /** Lightweight, fence-safe timestamp collector for the main GPU passes. */
    class GpuTimestampProfiler final {
    public:
        void create(VkPhysicalDevice physicalDevice, VkDevice device);
        void destroy() noexcept;
        void beginFrame(VkCommandBuffer commandBuffer, std::uint32_t frameIndex) const;
        void markSubmitted(std::uint32_t frameIndex) noexcept;
        [[nodiscard]] bool hasCompletedFrame() const noexcept;
        void beginPass(VkCommandBuffer commandBuffer, std::uint32_t frameIndex, GpuProfilePass pass) const;
        void endPass(VkCommandBuffer commandBuffer, std::uint32_t frameIndex, GpuProfilePass pass) const;
        [[nodiscard]] std::optional<GpuProfileFrame> completedFrame(std::uint32_t frameIndex) const;

    private:
        static constexpr std::uint32_t FramesInFlight = 2;
        static constexpr std::uint32_t PassCount = static_cast<std::uint32_t>(GpuProfilePass::Count);
        static constexpr std::uint32_t QueriesPerFrame = PassCount * 2;
        [[nodiscard]] static constexpr std::uint32_t query(std::uint32_t frame, GpuProfilePass pass,
                                                            bool end) noexcept {
            return frame * QueriesPerFrame + static_cast<std::uint32_t>(pass) * 2 + (end ? 1u : 0u);
        }

        VkDevice device_{VK_NULL_HANDLE};
        VkQueryPool queryPool_{VK_NULL_HANDLE};
        float timestampPeriodNs_{};
        std::array<bool, FramesInFlight> submitted_{};
        mutable bool hasCompletedFrame_{};
    };
} // namespace Engine
