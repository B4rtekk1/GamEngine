#include "Editor/Panels/ConsolePanel.h"
#include "Elements/NumericControl.h"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct LogEntry final {
    Editor::LogLevel level;
    std::string time;
    std::string message;
};

std::mutex logMutex;
std::vector<LogEntry> entries;
constexpr std::size_t maximumEntries = 2'000;

const char* levelName(const Editor::LogLevel level) {
    switch (level) {
    case Editor::LogLevel::Info: return "Info";
    case Editor::LogLevel::Warning: return "Warning";
    case Editor::LogLevel::Error: return "Error";
    }
    return "Unknown";
}

ImVec4 levelColor(const Editor::LogLevel level) {
    switch (level) {
    case Editor::LogLevel::Info: return {0.65F, 0.78F, 0.92F, 1.0F};
    case Editor::LogLevel::Warning: return {0.96F, 0.72F, 0.28F, 1.0F};
    case Editor::LogLevel::Error: return {0.98F, 0.38F, 0.35F, 1.0F};
    }
    return {1.0F, 1.0F, 1.0F, 1.0F};
}

bool matchesFilter(const LogEntry& entry, const char* filter) {
    if (filter == nullptr || *filter == '\0') return true;
    std::string message = entry.time + " " + levelName(entry.level) + " " + entry.message;
    std::string query{filter};
    std::ranges::transform(message, message.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    std::ranges::transform(query, query.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return message.find(query) != std::string::npos;
}

std::string currentTime() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
    localtime_s(&local, &now);
    char result[16]{};
    std::strftime(result, sizeof(result), "%H:%M:%S", &local);
    return result;
}

} // namespace

namespace Editor {

void ConsolePanel::add(const LogLevel level, const std::string_view message) {
    std::scoped_lock lock{logMutex};
    if (entries.size() == maximumEntries) entries.erase(entries.begin());
    entries.push_back({.level = level, .time = currentTime(), .message = std::string{message}});
}

void ConsolePanel::info(const std::string_view message) { add(LogLevel::Info, message); }
void ConsolePanel::warning(const std::string_view message) { add(LogLevel::Warning, message); }
void ConsolePanel::error(const std::string_view message) { add(LogLevel::Error, message); }

void ConsolePanel::clear() {
    std::scoped_lock lock{logMutex};
    entries.clear();
}

void ConsolePanel::draw(bool& isOpen) {
    if (!ImGui::Begin("Console", &isOpen)) {
        ImGui::End();
        return;
    }

    static bool showInfo = true;
    static bool showWarnings = true;
    static bool showErrors = true;
    static bool autoScroll = true;
    static float textScale = 1.0F;
    static char filter[256]{};
    if (ImGui::Button("Clear")) clear();
    ImGui::SameLine();
    ImGui::Checkbox("Info", &showInfo);
    ImGui::SameLine();
    ImGui::Checkbox("Warnings", &showWarnings);
    ImGui::SameLine();
    ImGui::Checkbox("Errors", &showErrors);
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &autoScroll);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0F);
    Editor::Controls::sliderFloat("Text size", &textScale, 0.75F, 2.0F, "x%.2f",
                                  ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Changes the size of log entries.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::InputTextWithHint("##console-filter", "Filter logs...", filter, sizeof(filter));
    ImGui::Separator();

    std::vector<LogEntry> visibleEntries;
    {
        std::scoped_lock lock{logMutex};
        visibleEntries = entries;
    }
    ImGui::BeginChild("##console-output", {0.0F, 0.0F}, false,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::SetWindowFontScale(textScale);
    for (const LogEntry& entry : visibleEntries) {
        const bool enabled = entry.level == LogLevel::Info ? showInfo
                             : entry.level == LogLevel::Warning ? showWarnings : showErrors;
        if (!enabled || !matchesFilter(entry, filter)) continue;
        ImGui::TextDisabled("[%s]", entry.time.c_str());
        ImGui::SameLine();
        ImGui::TextColored(levelColor(entry.level), "%-7s", levelName(entry.level));
        ImGui::SameLine();
        ImGui::TextUnformatted(entry.message.c_str());
    }
    if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0F) {
        ImGui::SetScrollHereY(1.0F);
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace Editor
