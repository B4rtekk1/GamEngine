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

    struct ProfileFrame final {
        std::uint64_t frameNumber{};
        double cpuFrameMs{};
        double gpuFrameMs{};
        std::vector<CpuProfileEvent> cpuEvents;
        std::vector<GpuProfileEvent> gpuEvents;
        bool gpuReady{};
    };

    /** Frame-local CPU profiler with a bounded, allocation-free-after-warmup history. */
    class Profiler final {
    public:
        static constexpr std::uint32_t HistorySize = 600;

        static ProfileNameId registerName(std::string_view name);
        [[nodiscard]] static std::string_view name(ProfileNameId id) noexcept;

        static void beginFrame();
        static void endFrame();
        static void beginCpuZone(ProfileNameId name);
        static void endCpuZone();
        /** Associates a fence-completed GPU timeline with its original CPU frame. */
        static void attachGpuFrame(std::uint64_t frameNumber, double milliseconds,
                                   std::span<const GpuProfileEvent> events) noexcept;
        static void clear();

        [[nodiscard]] static std::uint64_t currentFrameNumber() noexcept;
        [[nodiscard]] static std::uint32_t historySize() noexcept;
        /** Oldest frame is index zero. This view never copies profiling events. */
        [[nodiscard]] static const ProfileFrame& historyFrame(std::uint32_t index) noexcept;
    };

    class CpuProfileScope final {
    public:
        explicit CpuProfileScope(ProfileNameId name) { Profiler::beginCpuZone(name); }
        ~CpuProfileScope() { Profiler::endCpuZone(); }
        CpuProfileScope(const CpuProfileScope&) = delete;
        CpuProfileScope& operator=(const CpuProfileScope&) = delete;
    };
}

#define GE_PROFILE_CONCAT_INNER(a, b) a##b
#define GE_PROFILE_CONCAT(a, b) GE_PROFILE_CONCAT_INNER(a, b)
#define GE_PROFILE_SCOPE_IMPL(name, line) \
    static const ::Engine::ProfileNameId GE_PROFILE_CONCAT(_geProfileName_, line) = \
        ::Engine::Profiler::registerName(name); \
    ::Engine::CpuProfileScope GE_PROFILE_CONCAT(_geProfileScope_, line){ \
        GE_PROFILE_CONCAT(_geProfileName_, line)}
#define GE_PROFILE_SCOPE(name) GE_PROFILE_SCOPE_IMPL(name, __LINE__)
#define GE_PROFILE_FUNCTION() GE_PROFILE_SCOPE(__func__)
