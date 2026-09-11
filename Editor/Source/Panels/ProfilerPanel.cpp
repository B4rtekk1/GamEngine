#include "Editor/Panels/ProfilerPanel.h"

#include "Engine/Core/Profiler.h"
#include "Engine/Renderer/Renderer.h"
#include "imgui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace Editor {
namespace {
    const char* memoryCategoryName(const Engine::GpuMemoryCategory category) {
        switch (category) {
            case Engine::GpuMemoryCategory::DeviceLocal: return "Device local";
            case Engine::GpuMemoryCategory::HostVisible: return "Host visible";
            case Engine::GpuMemoryCategory::Other: return "Other";
        }
        return "Unknown";
    }

    double mib(const VkDeviceSize bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

    void drawMemoryInsights(const Engine::Renderer& renderer) {
        const auto categories = renderer.gpuMemoryCategories();
        const auto heaps = renderer.gpuMemoryHeaps();
        const auto gpuScene = renderer.gpuSceneMemory();
        ImGui::Separator(); ImGui::TextUnformatted("MEMORY INSIGHTS");
        if (heaps.empty()) { ImGui::TextDisabled("GPU memory budget is unavailable before Vulkan initialization."); return; }
        if (ImGui::BeginTable("##memory-categories", 5, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Category"); ImGui::TableSetupColumn("Usage"); ImGui::TableSetupColumn("Budget"); ImGui::TableSetupColumn("Allocations"); ImGui::TableSetupColumn("Pressure"); ImGui::TableHeadersRow();
            for (const auto& category : categories) { if (!category.budget && !category.usage && !category.allocationCount) continue; ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(memoryCategoryName(category.category)); ImGui::TableSetColumnIndex(1); ImGui::Text("%.1f MiB", mib(category.usage)); ImGui::TableSetColumnIndex(2); ImGui::Text("%.1f MiB", mib(category.budget)); ImGui::TableSetColumnIndex(3); ImGui::Text("%u", category.allocationCount); ImGui::TableSetColumnIndex(4); ImGui::Text("%.1f%%", category.budget ? 100.0 * static_cast<double>(category.usage) / static_cast<double>(category.budget) : 0.0); }
            ImGui::EndTable();
        }
        if (ImGui::TreeNode("Heap details")) { if (ImGui::BeginTable("##memory-heaps", 7, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) { ImGui::TableSetupColumn("Heap"); ImGui::TableSetupColumn("Class"); ImGui::TableSetupColumn("Usage"); ImGui::TableSetupColumn("Budget"); ImGui::TableSetupColumn("Allocated"); ImGui::TableSetupColumn("Allocations"); ImGui::TableSetupColumn("Blocks"); ImGui::TableHeadersRow(); for (const auto& heap : heaps) { ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Text("%u", heap.heapIndex); ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(memoryCategoryName(heap.category)); ImGui::TableSetColumnIndex(2); ImGui::Text("%.1f MiB", mib(heap.usage)); ImGui::TableSetColumnIndex(3); ImGui::Text("%.1f MiB", mib(heap.budget)); ImGui::TableSetColumnIndex(4); ImGui::Text("%.1f MiB", mib(heap.allocationBytes)); ImGui::TableSetColumnIndex(5); ImGui::Text("%u", heap.allocationCount); ImGui::TableSetColumnIndex(6); ImGui::Text("%u", heap.blockCount); } ImGui::EndTable(); } ImGui::TreePop(); }
        if (ImGui::TreeNode("GPU Scene memory")) {
            if (gpuScene.empty()) ImGui::TextDisabled("GPU Scene buffers are not allocated.");
            else if (ImGui::BeginTable("##gpu-scene-memory", 6, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Table"); ImGui::TableSetupColumn("Heap"); ImGui::TableSetupColumn("Device local"); ImGui::TableSetupColumn("Host visible"); ImGui::TableSetupColumn("Host coherent"); ImGui::TableSetupColumn("Bytes"); ImGui::TableHeadersRow();
                for (const auto& allocation : gpuScene) { ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(allocation.table.c_str()); ImGui::TableSetColumnIndex(1); ImGui::Text("%u", allocation.heapIndex); ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(allocation.deviceLocal ? "yes" : "no"); ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(allocation.hostVisible ? "yes" : "no"); ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(allocation.hostCoherent ? "yes" : "no"); ImGui::TableSetColumnIndex(5); ImGui::Text("%llu", static_cast<unsigned long long>(allocation.bytes)); }
                ImGui::EndTable();
            }
            ImGui::TreePop();
        }
    }

    struct Hotspot final {
        double self{};
        double total{};
        double max{};
        std::vector<double> samples;
        std::uint32_t calls{};
    };

    double eventMilliseconds(const Engine::CpuProfileEvent& event) {
        return static_cast<double>(event.endNs - event.startNs) * 1.0e-6;
    }

    double selfMilliseconds(const Engine::ProfileFrame& frame, const std::size_t index) {
        const auto& event = frame.cpuEvents[index];
        std::uint64_t childNs{};
        for (std::size_t child = index + 1; child < frame.cpuEvents.size(); ++child) {
            const auto& candidate = frame.cpuEvents[child];
            if (candidate.depth <= event.depth) break;
            if (candidate.depth == event.depth + 1) childNs += candidate.endNs - candidate.startNs;
        }
        return std::max(0.0, eventMilliseconds(event) - static_cast<double>(childNs) * 1.0e-6);
    }

    ImU32 zoneColor(const Engine::ProfileNameId name) {
        const std::uint32_t hue = (name * 57U + 121U) % 360U;
        ImVec4 color;
        ImGui::ColorConvertHSVtoRGB(static_cast<float>(hue) / 360.0F, 0.58F, 0.82F, color.x, color.y, color.z);
        color.w = 1.0F;
        return ImGui::ColorConvertFloat4ToU32(color);
    }

    void drawTimeline(const char* label, const Engine::ProfileFrame& frame, const bool gpu) {
        ImGui::TextUnformatted(label);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        constexpr float rowHeight = 20.0F;
        constexpr float labelWidth = 105.0F;
        constexpr float height = 104.0F;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin, {origin.x + width, origin.y + height}, IM_COL32(20, 23, 30, 255), 3.0F);
        const double frameLength = gpu ? std::max(frame.gpuFrameMs, 0.001) : std::max(frame.cpuFrameMs, 0.001);
        const auto drawEvent = [&](const auto& event, const double start, const double end, const std::uint16_t depth) {
            const float y = origin.y + 4.0F + std::min<float>(depth, 4.0F) * rowHeight;
            const float x0 = origin.x + labelWidth + static_cast<float>(start / frameLength) * (width - labelWidth - 4.0F);
            const float x1 = std::max(x0 + 2.0F, origin.x + labelWidth + static_cast<float>(end / frameLength) * (width - labelWidth - 4.0F));
            draw->AddRectFilled({x0, y}, {x1, y + rowHeight - 3.0F}, zoneColor(event.name), 2.0F);
            if (x1 - x0 > 42.0F) draw->AddText({x0 + 4.0F, y + 2.0F}, IM_COL32(245, 245, 245, 255), Engine::Profiler::name(event.name).data());
            if (ImGui::IsMouseHoveringRect({x0, y}, {x1, y + rowHeight - 3.0F}))
                ImGui::SetTooltip("%s: %.3f ms", Engine::Profiler::name(event.name).data(), end - start);
        };
        if (gpu) {
            for (const auto& event : frame.gpuEvents) drawEvent(event, event.startMs, event.endMs, event.depth);
        } else {
            for (const auto& event : frame.cpuEvents) drawEvent(event, event.startNs * 1.0e-6, event.endNs * 1.0e-6, event.depth);
        }
        ImGui::Dummy({width, height});
    }

    void drawFrameGraph(const std::uint32_t count, std::uint64_t& selected, bool& followLatest) {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        constexpr float height = 156.0F;
        float maximum = 33.334F;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto& frame = Engine::Profiler::historyFrame(i);
            maximum = std::max(maximum, static_cast<float>(std::max(frame.cpuFrameMs, frame.gpuReady ? frame.gpuFrameMs : 0.0)));
        }
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin, {origin.x + width, origin.y + height}, IM_COL32(20, 23, 30, 255), 3.0F);
        for (const auto [time, name] : std::array<std::pair<float, const char*>, 3>{{{8.333F, "120 FPS"}, {16.667F, "60 FPS"}, {33.333F, "30 FPS"}}}) {
            const float y = origin.y + height - time / maximum * height;
            draw->AddLine({origin.x, y}, {origin.x + width, y}, IM_COL32(130, 130, 145, 95));
            draw->AddText({origin.x + 4.0F, y - 15.0F}, IM_COL32(180, 180, 190, 190), name);
        }
        const float step = width / std::max(1U, count - 1U);
        for (std::uint32_t i = 1; i < count; ++i) {
            const auto& a = Engine::Profiler::historyFrame(i - 1);
            const auto& b = Engine::Profiler::historyFrame(i);
            const float x0 = origin.x + step * (i - 1); const float x1 = origin.x + step * i;
            draw->AddLine({x0, origin.y + height - static_cast<float>(a.cpuFrameMs) / maximum * height}, {x1, origin.y + height - static_cast<float>(b.cpuFrameMs) / maximum * height}, IM_COL32(80, 190, 255, 255), 2.0F);
            if (a.gpuReady && b.gpuReady) draw->AddLine({x0, origin.y + height - static_cast<float>(a.gpuFrameMs) / maximum * height}, {x1, origin.y + height - static_cast<float>(b.gpuFrameMs) / maximum * height}, IM_COL32(255, 170, 65, 255), 2.0F);
        }
        ImGui::InvisibleButton("##frame-history", {width, height});
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        if (ImGui::IsItemHovered()) {
            const std::uint32_t index = std::min(count - 1U, static_cast<std::uint32_t>(std::max(0.0F, (mouse.x - origin.x) / width) * count));
            const auto& frame = Engine::Profiler::historyFrame(index);
            ImGui::SetTooltip("Frame %llu\nCPU: %.3f ms\nGPU: %s", static_cast<unsigned long long>(frame.frameNumber), frame.cpuFrameMs, frame.gpuReady ? (std::to_string(frame.gpuFrameMs) + " ms").c_str() : "pending");
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { selected = frame.frameNumber; followLatest = false; }
        }
        ImGui::TextDisabled("CPU"); ImGui::SameLine(); ImGui::TextColored({0.31F, .75F, 1.0F, 1.0F}, "—"); ImGui::SameLine(); ImGui::TextDisabled("GPU"); ImGui::SameLine(); ImGui::TextColored({1.0F, .65F, .25F, 1.0F}, "—");
    }
}

void drawProfilerPanel(const Engine::Renderer& renderer, bool& isOpen) {
    if (!isOpen) return;
    if (!ImGui::Begin("Profiler", &isOpen)) { ImGui::End(); return; }
    static bool followLatest = true;
    static std::uint64_t selectedFrame{};
    static int aggregationFrames = 300;
    if (ImGui::Button(followLatest ? "Lock frame" : "Follow latest")) followLatest = !followLatest;
    ImGui::SameLine(); if (ImGui::Button("Clear")) { Engine::Profiler::clear(); selectedFrame = 0; }
    const std::uint32_t count = Engine::Profiler::historySize();
    drawMemoryInsights(renderer);
    if (!count) { ImGui::TextDisabled("Waiting for profiled frames..."); ImGui::End(); return; }
    if (followLatest || !selectedFrame) selectedFrame = Engine::Profiler::historyFrame(count - 1).frameNumber;
    const Engine::ProfileFrame* selected = &Engine::Profiler::historyFrame(count - 1);
    for (std::uint32_t i = 0; i < count; ++i) if (Engine::Profiler::historyFrame(i).frameNumber == selectedFrame) selected = &Engine::Profiler::historyFrame(i);
    ImGui::Text("CPU %.3f ms   GPU %s   %.1f FPS", selected->cpuFrameMs, selected->gpuReady ? (std::to_string(selected->gpuFrameMs) + " ms").c_str() : "pending", selected->cpuFrameMs > 0.0 ? 1000.0 / selected->cpuFrameMs : 0.0);
    ImGui::Separator(); ImGui::TextUnformatted("FRAME TIME"); drawFrameGraph(count, selectedFrame, followLatest);
    ImGui::Separator(); drawTimeline("CPU TIMELINE", *selected, false);
    if (selected->gpuReady) drawTimeline("GPU TIMELINE", *selected, true);
    ImGui::Separator(); ImGui::TextUnformatted("HOTSPOTS"); ImGui::SameLine(); ImGui::SetNextItemWidth(100.0F); ImGui::SliderInt("frames", &aggregationFrames, 1, static_cast<int>(count));
    const std::uint32_t first = count - std::min<std::uint32_t>(count, aggregationFrames);
    std::map<Engine::ProfileNameId, Hotspot> hotspots;
    for (std::uint32_t f = first; f < count; ++f) { const auto& frame = Engine::Profiler::historyFrame(f); for (std::size_t i = 0; i < frame.cpuEvents.size(); ++i) { const double total = eventMilliseconds(frame.cpuEvents[i]); const double self = selfMilliseconds(frame, i); auto& h = hotspots[frame.cpuEvents[i].name]; h.total += total; h.self += self; h.max = std::max(h.max, self); h.samples.push_back(self); ++h.calls; } }
    std::vector<std::pair<Engine::ProfileNameId, Hotspot*>> ordered; for (auto& [name, hotspot] : hotspots) ordered.emplace_back(name, &hotspot);
    std::ranges::sort(ordered, [](const auto& a, const auto& b) { return a.second->self > b.second->self; });
    if (ImGui::BeginTable("##hotspots", 6, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) { ImGui::TableSetupColumn("Zone"); ImGui::TableSetupColumn("Avg self"); ImGui::TableSetupColumn("Avg total"); ImGui::TableSetupColumn("p95"); ImGui::TableSetupColumn("Max"); ImGui::TableSetupColumn("Calls/frame"); ImGui::TableHeadersRow(); const double divisor = static_cast<double>(count - first); for (auto [name, h] : ordered) { std::ranges::sort(h->samples); const double p95 = h->samples[std::min(h->samples.size() - 1, static_cast<std::size_t>(std::ceil(h->samples.size() * .95) - 1))]; ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(Engine::Profiler::name(name).data()); ImGui::TableSetColumnIndex(1); ImGui::Text("%.3f", h->self / divisor); ImGui::TableSetColumnIndex(2); ImGui::Text("%.3f", h->total / divisor); ImGui::TableSetColumnIndex(3); ImGui::Text("%.3f", p95); ImGui::TableSetColumnIndex(4); ImGui::Text("%.3f", h->max); ImGui::TableSetColumnIndex(5); ImGui::Text("%.1f", h->calls / divisor); } ImGui::EndTable(); }
    ImGui::End();
}
} // namespace Editor
