#include "Engine/Core/Profiler.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace Engine {
namespace {
    using Clock = std::chrono::steady_clock;
    constexpr std::size_t MaxEventsPerFrame = 1024;
    constexpr std::size_t MaxGpuEventsPerFrame = 64;
    struct ActiveZone final { std::size_t eventIndex; };
    struct Storage final {
        std::array<ProfileFrame, Profiler::HistorySize> frames;
        std::deque<std::string> names{"<unknown>"};
        std::mutex namesMutex;
        std::uint32_t nextFrame{};
        std::uint32_t frameCount{};
        std::uint64_t frameNumber{};
        bool recording{};
        bool clearRequested{};
        Clock::time_point frameStart{};
        std::thread::id recordingThread{};

        Storage() {
            for (ProfileFrame& frame : frames) {
                frame.cpuEvents.reserve(MaxEventsPerFrame);
                frame.gpuEvents.reserve(MaxGpuEventsPerFrame);
            }
        }
    };
    Storage& storage() { static Storage value; return value; }
    thread_local std::vector<ActiveZone> activeZones;
    std::uint64_t nowNs(const Clock::time_point origin) {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - origin).count());
    }
}

ProfileNameId Profiler::registerName(const std::string_view value) {
    Storage& state = storage();
    std::scoped_lock lock{state.namesMutex};
    const auto found = std::find(state.names.begin(), state.names.end(), value);
    if (found != state.names.end()) return static_cast<ProfileNameId>(found - state.names.begin());
    state.names.emplace_back(value);
    return static_cast<ProfileNameId>(state.names.size() - 1);
}

std::string_view Profiler::name(const ProfileNameId id) noexcept {
    Storage& state = storage();
    std::scoped_lock lock{state.namesMutex};
    return id < state.names.size() ? std::string_view{state.names[id]} : std::string_view{"<unknown>"};
}

void Profiler::beginFrame() {
    Storage& state = storage();
    if (state.clearRequested) {
        for (ProfileFrame& frame : state.frames) {
            frame.frameNumber = 0;
            frame.cpuFrameMs = 0.0;
            frame.gpuFrameMs = 0.0;
            frame.cpuEvents.clear();
            frame.gpuEvents.clear();
            frame.gpuReady = false;
        }
        state.nextFrame = 0;
        state.frameCount = 0;
        state.clearRequested = false;
    }
    ProfileFrame& frame = state.frames[state.nextFrame];
    frame.frameNumber = ++state.frameNumber;
    frame.cpuFrameMs = 0.0;
    frame.gpuFrameMs = 0.0;
    frame.cpuEvents.clear();
    frame.gpuEvents.clear();
    frame.gpuReady = false;
    state.frameStart = Clock::now();
    state.recording = true;
    state.recordingThread = std::this_thread::get_id();
    activeZones.clear();
}

void Profiler::endFrame() {
    Storage& state = storage();
    if (!state.recording) return;
    const std::uint64_t end = nowNs(state.frameStart);
    ProfileFrame& frame = state.frames[state.nextFrame];
    while (!activeZones.empty()) {
        frame.cpuEvents[activeZones.back().eventIndex].endNs = end;
        activeZones.pop_back();
    }
    frame.cpuFrameMs = static_cast<double>(end) * 1.0e-6;
    state.nextFrame = (state.nextFrame + 1) % HistorySize;
    state.frameCount = std::min(state.frameCount + 1, HistorySize);
    state.recording = false;
    state.recordingThread = {};
}

void Profiler::beginCpuZone(const ProfileNameId name) {
    Storage& state = storage();
    if (!state.recording || std::this_thread::get_id() != state.recordingThread) return;
    ProfileFrame& frame = state.frames[state.nextFrame];
    if (frame.cpuEvents.size() == MaxEventsPerFrame) return;
    frame.cpuEvents.push_back({name, nowNs(state.frameStart), 0,
        static_cast<std::uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id())),
        static_cast<std::uint16_t>(activeZones.size())});
    activeZones.push_back({frame.cpuEvents.size() - 1});
}

void Profiler::endCpuZone() {
    Storage& state = storage();
    if (!state.recording || std::this_thread::get_id() != state.recordingThread || activeZones.empty()) return;
    state.frames[state.nextFrame].cpuEvents[activeZones.back().eventIndex].endNs = nowNs(state.frameStart);
    activeZones.pop_back();
}

void Profiler::attachGpuFrame(const std::uint64_t frameNumber, const double milliseconds,
                              const std::span<const GpuProfileEvent> events) noexcept {
    Storage& state = storage();
    for (ProfileFrame& frame : state.frames) {
        if (frame.frameNumber != frameNumber) continue;
        frame.gpuFrameMs = milliseconds;
        frame.gpuEvents.assign(events.begin(), events.end());
        frame.gpuReady = true;
        return;
    }
}

void Profiler::clear() {
    Storage& state = storage();
    state.clearRequested = true;
}

std::uint64_t Profiler::currentFrameNumber() noexcept { return storage().frameNumber; }

std::uint32_t Profiler::historySize() noexcept { return storage().frameCount; }

const ProfileFrame& Profiler::historyFrame(const std::uint32_t index) noexcept {
    Storage& state = storage();
    const std::uint32_t first = state.frameCount == HistorySize ? state.nextFrame : 0;
    if (state.frameCount == 0) return state.frames[0];
    return state.frames[(first + std::min(index, state.frameCount - 1)) % HistorySize];
}
} // namespace Engine
