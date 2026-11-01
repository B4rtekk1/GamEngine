#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace Engine {
using ProfileNameId = std::uint32_t;

struct CpuProfileEvent final {
    ProfileNameId name{};
    std::uint64_t startNs{};
    std::uint64_t endNs{};
    std::uint32_t threadId{};
    std::uint16_t depth{};
};

struct GpuProfileEvent final {
    ProfileNameId name{};
    float startMs{};
    float endMs{};
    std::uint16_t depth{};
};

struct RenderGraphProfileStats final {
    std::uint32_t passes{};
    std::uint32_t graphicsBatches{};
    std::uint32_t computeBatches{};
    std::uint32_t transferBatches{};
    std::uint32_t crossQueueEdges{};
    std::uint32_t pipelineBarriers{};
    std::uint32_t imageBarriers{};
    std::uint32_t bufferBarriers{};
    std::uint32_t ownershipTransfers{};
};

struct ProfileFrame final {
    std::uint64_t frameNumber{};
    double cpuFrameMs{};
    double gpuFrameMs{};
    std::vector<CpuProfileEvent> cpuEvents;
    std::vector<GpuProfileEvent> gpuEvents;
    RenderGraphProfileStats renderGraph{};
    bool gpuReady{};
};

/** Frame-local CPU profiler with a bounded, allocation-free-after-warmup history. */
class Profiler final {
  public:
    static constexpr std::uint32_t HistorySize = 600;

    static ProfileNameId registerName(std::string_view name);

    [[nodiscard]] static std::string_view name(ProfileNameId id) noexcept;

    /**
     * @brief Enables or disables per-frame event recording.
     *
     * Recording costs a clock read plus a vector push for every
     * GE_PROFILE_SCOPE and reserves roughly twenty megabytes of history, so a
     * shipping frame should not pay for data nobody displays. Disabling stops
     * all CPU zones and lets the GPU timestamp pool skip its query writes;
     * frame numbers keep advancing so late GPU results still attach to the
     * right frame once recording is turned back on.
     */
    static void setEnabled(bool enabled) noexcept;

    /** @brief Whether per-frame event recording is currently active. */
    [[nodiscard]] static bool enabled() noexcept;

    static void beginFrame();

    static void endFrame();

    static void beginCpuZone(ProfileNameId name);

    static void endCpuZone();

    /** Associates a fence-completed GPU timeline with its original CPU frame. */
    static void attachGpuFrame(std::uint64_t frameNumber, double milliseconds,
                               std::span<const GpuProfileEvent> events) noexcept;
    static void addRenderGraphStats(const RenderGraphProfileStats& stats) noexcept;

    static void clear();

    [[nodiscard]] static std::uint64_t currentFrameNumber() noexcept;

    [[nodiscard]] static std::uint32_t historySize() noexcept;

    /** Oldest frame is index zero. This view never copies profiling events. */
    [[nodiscard]] static const ProfileFrame& historyFrame(std::uint32_t index) noexcept;
};

class CpuProfileScope final {
  public:
    explicit CpuProfileScope(ProfileNameId name) {
        Profiler::beginCpuZone(name);
    }
    ~CpuProfileScope() {
        Profiler::endCpuZone();
    }

    CpuProfileScope(const CpuProfileScope&) = delete;

    CpuProfileScope& operator=(const CpuProfileScope&) = delete;
};
} // namespace Engine

#define GE_PROFILE_CONCAT_INNER(a, b) a##b
#define GE_PROFILE_CONCAT(a, b) GE_PROFILE_CONCAT_INNER(a, b)
#define GE_PROFILE_SCOPE_IMPL(name, line)                                                                              \
    static const ::Engine::ProfileNameId GE_PROFILE_CONCAT(_geProfileName_, line) =                                    \
        ::Engine::Profiler::registerName(name);                                                                        \
    ::Engine::CpuProfileScope GE_PROFILE_CONCAT(_geProfileScope_, line) {                                              \
        GE_PROFILE_CONCAT(_geProfileName_, line)                                                                       \
    }
#define GE_PROFILE_SCOPE(name) GE_PROFILE_SCOPE_IMPL(name, __LINE__)
#define GE_PROFILE_FUNCTION() GE_PROFILE_SCOPE(__func__)
