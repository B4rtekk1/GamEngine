#include "Editor/Panels/ConsolePanel.h"
#include "Elements/NumericControl.h"
#include "Engine/Core/Diagnostics.h"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <limits>
#include <string>
#include <vector>

namespace {

Editor::LogLevel toLogLevel(const Engine::DiagnosticSeverity level) {
    switch (level) {
    case Engine::DiagnosticSeverity::Info: return Editor::LogLevel::Info;
    case Engine::DiagnosticSeverity::Warning: return Editor::LogLevel::Warning;
    case Engine::DiagnosticSeverity::Error: return Editor::LogLevel::Error;
    }
    return Editor::LogLevel::Error;
}

std::string formatDiagnostic(const Engine::Diagnostic &diagnostic) {
    std::string result = diagnostic.message;
    const auto append = [&](const char *label, const std::string &value) {
        if (!value.empty()) result += " | " + std::string{label} + ": " + value;
    };
    append("System", diagnostic.context.subsystem);
    append("Object", diagnostic.context.object);
    append("Component", diagnostic.context.component);
    append("File", diagnostic.context.file);
    append("Suggested action", diagnostic.context.suggestedAction);
    return result;
}

const char* levelName(const Editor::LogLevel level) {
    switch (level) {
    case Editor::LogLevel::Info: return "Info";
    case Editor::LogLevel::Warning: return "Warning";
    case Editor::LogLevel::Error: return "Error";
    }
    return "Unknown";
}

std::string formatTime(const std::chrono::system_clock::time_point timestamp) {
    const auto time = std::chrono::system_clock::to_time_t(timestamp);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    char result[16]{};
    std::strftime(result, sizeof(result), "%H:%M:%S", &local);
    return result;
}

bool matchesFilter(const Engine::Diagnostic& entry, const char* filter) {
    if (filter == nullptr || *filter == '\0') return true;
    std::string message = formatTime(entry.timestamp) + " " + levelName(toLogLevel(entry.severity)) + " " +
                          formatDiagnostic(entry);
    std::string query{filter};
    std::ranges::transform(message, message.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    std::ranges::transform(query, query.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return message.find(query) != std::string::npos;
}

} // namespace

namespace Editor {

void ConsolePanel::add(const LogLevel level, const std::string_view message) {
    const auto severity = level == LogLevel::Info ? Engine::DiagnosticSeverity::Info
                        : level == LogLevel::Warning ? Engine::DiagnosticSeverity::Warning
                                                     : Engine::DiagnosticSeverity::Error;
    Engine::Diagnostics::instance().report(severity, std::string{message}, {.subsystem = "Editor"});
}

void ConsolePanel::info(const std::string_view message) { add(LogLevel::Info, message); }
void ConsolePanel::warning(const std::string_view message) { add(LogLevel::Warning, message); }
void ConsolePanel::error(const std::string_view message) { add(LogLevel::Error, message); }

void ConsolePanel::clear() {
    Engine::Diagnostics::instance().clear();
}

void ConsolePanel::draw(bool& isOpen) {
    if (!ImGui::Begin("Console", &isOpen)) {
        ImGui::End();
        return;
    }

    const auto diagnostics = Engine::Diagnostics::instance().entries();

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

    // ImGui::TextUnformatted does not expose text selection.  A read-only
    // multiline input does, while still keeping the console immutable.
    std::string output;
    for (const Engine::Diagnostic& entry : diagnostics) {
        const LogLevel level = toLogLevel(entry.severity);
        const bool enabled = level == LogLevel::Info ? showInfo
                             : level == LogLevel::Warning ? showWarnings : showErrors;
        if (!enabled || !matchesFilter(entry, filter)) continue;
        output += '[' + formatTime(entry.timestamp) + "] ";
        output += levelName(level);
        output += "  ";
        output += formatDiagnostic(entry);
        output += '\n';
    }

    // Keep a null terminator even when there are no visible log entries.
    static std::vector<char> selectableOutput;
    selectableOutput.assign(output.begin(), output.end());
    selectableOutput.push_back('\0');

    // SetWindowFontScale is a legacy per-window API. In particular, it does
    // not reliably update the child window used by InputTextMultiline.
    // Select the scaled font explicitly so the log text, line height and
    // scrolling area all use the same size.
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * textScale);
    ImGui::InputTextMultiline("##console-output", selectableOutput.data(), selectableOutput.size(),
                              {-std::numeric_limits<float>::min(), -std::numeric_limits<float>::min()},
                              ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_AllowTabInput);
    ImGui::PopFont();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Drag to select text. Ctrl+C copies the selection.");
    }
    ImGui::End();
}

} // namespace Editor
