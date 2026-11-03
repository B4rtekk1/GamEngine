#include "Editor/Panels/ConsolePanel.h"

#include "Editor/UI/EditorIcons.h"
#include "Editor/UI/EditorTheme.h"
#include "Editor/UI/EditorWidgets.h"
#include "Elements/NumericControl.h"
#include "Engine/Core/Diagnostics.h"
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <limits>
#include <string>
#include <vector>

namespace {

Editor::LogLevel toLogLevel(const Engine::DiagnosticSeverity level) {
    switch (level) {
    case Engine::DiagnosticSeverity::Info:
        return Editor::LogLevel::Info;
    case Engine::DiagnosticSeverity::Warning:
        return Editor::LogLevel::Warning;
    case Engine::DiagnosticSeverity::Error:
        return Editor::LogLevel::Error;
    }
    return Editor::LogLevel::Error;
}

std::string formatDiagnostic(const Engine::Diagnostic& diagnostic) {
    std::string result = diagnostic.message;
    const auto append = [&](const char* label, const std::string& value) {
        if (!value.empty())
            result += " | " + std::string{label} + ": " + value;
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
    case Editor::LogLevel::Info:
        return "Info";
    case Editor::LogLevel::Warning:
        return "Warning";
    case Editor::LogLevel::Error:
        return "Error";
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
    if (filter == nullptr || *filter == '\0')
        return true;
    std::string message =
        formatTime(entry.timestamp) + " " + levelName(toLogLevel(entry.severity)) + " " + formatDiagnostic(entry);
    std::string query{filter};
    std::ranges::transform(message, message.begin(),
                           [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    std::ranges::transform(query, query.begin(),
                           [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return message.find(query) != std::string::npos;
}

EditorUI::Icon levelIcon(const Editor::LogLevel level) {
    switch (level) {
    case Editor::LogLevel::Info:
        return EditorUI::Icon::Info;
    case Editor::LogLevel::Warning:
        return EditorUI::Icon::Warning;
    case Editor::LogLevel::Error:
        return EditorUI::Icon::Error;
    }
    return EditorUI::Icon::Error;
}

ImVec4 levelColor(const Editor::LogLevel level) {
    const EditorUI::Palette& palette = EditorUI::colors();
    switch (level) {
    case Editor::LogLevel::Info:
        return palette.textSecondary;
    case Editor::LogLevel::Warning:
        return palette.warning;
    case Editor::LogLevel::Error:
        return palette.error;
    }
    return palette.error;
}

/** Toolbar filter showing a level icon and its entry count. */
void levelToggle(const char* id, const Editor::LogLevel level, const std::size_t count, const char* tooltip,
                 bool& enabled) {
    const EditorUI::Palette& palette = EditorUI::colors();
    char countText[24]{};
    std::snprintf(countText, sizeof(countText), "%zu", count);
    const float height = ImGui::GetFrameHeight();
    const float iconSize = ImGui::GetFontSize() * 0.82F;
    const float width = 8.0F + iconSize + 5.0F + ImGui::CalcTextSize(countText).x + 9.0F;
    if (ImGui::InvisibleButton(id, {width, height}))
        enabled = !enabled;
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec4 color = levelColor(level);
    if (enabled || hovered) {
        drawList->AddRectFilled(
            min, max,
            ImGui::GetColorU32(enabled ? EditorUI::withAlpha(color, hovered ? 0.24F : 0.16F) : palette.controlHover),
            ImGui::GetStyle().FrameRounding);
    }
    const ImVec4 foreground = enabled ? color : EditorUI::withAlpha(palette.textSecondary, 0.6F);
    const float centerY = (min.y + max.y) * 0.5F;
    EditorUI::drawIcon(drawList, levelIcon(level), {min.x + 8.0F + iconSize * 0.5F, centerY}, iconSize,
                       ImGui::GetColorU32(foreground));
    drawList->AddText({min.x + 8.0F + iconSize + 5.0F, centerY - ImGui::GetTextLineHeight() * 0.5F},
                      ImGui::GetColorU32(enabled ? palette.textPrimary : palette.textSecondary), countText);
    if (hovered)
        ImGui::SetTooltip("%s", tooltip);
}

} // namespace

namespace Editor {

void ConsolePanel::add(const LogLevel level, const std::string_view message) {
    const auto severity = level == LogLevel::Info      ? Engine::DiagnosticSeverity::Info
                          : level == LogLevel::Warning ? Engine::DiagnosticSeverity::Warning
                                                       : Engine::DiagnosticSeverity::Error;
    Engine::Diagnostics::instance().report(severity, std::string{message}, {.subsystem = "Editor"});
}

void ConsolePanel::info(const std::string_view message) {
    add(LogLevel::Info, message);
}
void ConsolePanel::warning(const std::string_view message) {
    add(LogLevel::Warning, message);
}
void ConsolePanel::error(const std::string_view message) {
    add(LogLevel::Error, message);
}

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
    static bool textSelectionMode = false;
    static float textScale = 1.0F;
    static char filter[256]{};
    static std::size_t selectedEntry = std::numeric_limits<std::size_t>::max();
    static std::size_t lastVisibleCount = 0;

    std::size_t counts[3]{};
    for (const Engine::Diagnostic& entry : diagnostics)
        ++counts[static_cast<std::size_t>(toLogLevel(entry.severity))];

    if (EditorUI::iconButton("##console-clear", EditorUI::Icon::Trash, "Clear the console"))
        clear();
    EditorUI::toolbarSeparator();
    levelToggle("##console-info", LogLevel::Info, counts[0], "Show info messages", showInfo);
    ImGui::SameLine(0.0F, 2.0F);
    levelToggle("##console-warnings", LogLevel::Warning, counts[1], "Show warnings", showWarnings);
    ImGui::SameLine(0.0F, 2.0F);
    levelToggle("##console-errors", LogLevel::Error, counts[2], "Show errors", showErrors);
    EditorUI::toolbarSeparator();
    ImGui::Checkbox("Auto-scroll", &autoScroll);
    ImGui::SameLine();
    ImGui::Checkbox("Select text", &textSelectionMode);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Show the log as plain text so any part of it can be selected and copied.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0F);
    Editor::Controls::sliderFloat("##console-text-size", &textScale, 0.75F, 2.0F, "Text x%.2f",
                                  ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Changes the size of log entries.");
    ImGui::SameLine();
    EditorUI::searchBox("##console-filter", "Filter logs...", filter, sizeof(filter), -1.0F);

    std::vector<std::size_t> visible;
    visible.reserve(diagnostics.size());
    for (std::size_t index = 0; index < diagnostics.size(); ++index) {
        const Engine::Diagnostic& entry = diagnostics[index];
        const LogLevel level = toLogLevel(entry.severity);
        const bool enabled = level == LogLevel::Info      ? showInfo
                             : level == LogLevel::Warning ? showWarnings
                                                          : showErrors;
        if (enabled && matchesFilter(entry, filter))
            visible.push_back(index);
    }
    const auto formatLine = [&](const Engine::Diagnostic& entry) {
        return '[' + formatTime(entry.timestamp) + "] " + levelName(toLogLevel(entry.severity)) + "  " +
               formatDiagnostic(entry);
    };

    // SetWindowFontScale is a legacy per-window API. In particular, it does
    // not reliably update the child window used by InputTextMultiline.
    // Select the scaled font explicitly so the log text, line height and
    // scrolling area all use the same size.
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * textScale);
    if (textSelectionMode) {
        // ImGui::TextUnformatted does not expose text selection.  A read-only
        // multiline input does, while still keeping the console immutable.
        std::string output;
        for (const std::size_t index : visible) {
            output += formatLine(diagnostics[index]);
            output += '\n';
        }
        // Keep a null terminator even when there are no visible log entries.
        static std::vector<char> selectableOutput;
        selectableOutput.assign(output.begin(), output.end());
        selectableOutput.push_back('\0');
        ImGui::InputTextMultiline("##console-output", selectableOutput.data(), selectableOutput.size(),
                                  {-std::numeric_limits<float>::min(), -std::numeric_limits<float>::min()},
                                  ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_AllowTabInput);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Drag to select text. Ctrl+C copies the selection.");
    } else {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorUI::colors().appBackground);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {ImGui::GetStyle().ItemSpacing.x, 2.0F});
        if (ImGui::BeginChild("##console-list", {0.0F, 0.0F}, ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_HorizontalScrollbar)) {
            if (visible.empty()) {
                ImGui::TextDisabled(diagnostics.empty() ? "No messages yet." : "No messages match the filter.");
            }
            const float iconSize = ImGui::GetFontSize() * 0.8F;
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(visible.size()));
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    const std::size_t index = visible[static_cast<std::size_t>(row)];
                    const Engine::Diagnostic& entry = diagnostics[index];
                    const LogLevel level = toLogLevel(entry.severity);
                    ImGui::PushID(static_cast<int>(index));
                    const ImVec2 rowStart = ImGui::GetCursorScreenPos();
                    if (row % 2 == 1) {
                        ImGui::GetWindowDrawList()->AddRectFilled(
                            {ImGui::GetWindowPos().x, rowStart.y - 1.0F},
                            {ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                             rowStart.y + ImGui::GetTextLineHeight() + 1.0F},
                            ImGui::GetColorU32(EditorUI::withAlpha(EditorUI::colors().surfaceRaised, 0.35F)));
                    }
                    if (ImGui::Selectable("##console-row", selectedEntry == index, ImGuiSelectableFlags_AllowOverlap))
                        selectedEntry = index;
                    const bool rowHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal);
                    if (ImGui::BeginPopupContextItem("##console-row-menu")) {
                        selectedEntry = index;
                        if (ImGui::MenuItem("Copy message"))
                            ImGui::SetClipboardText(formatDiagnostic(entry).c_str());
                        if (ImGui::MenuItem("Copy line"))
                            ImGui::SetClipboardText(formatLine(entry).c_str());
                        if (ImGui::MenuItem("Copy all visible")) {
                            std::string all;
                            for (const std::size_t visibleIndex : visible)
                                all += formatLine(diagnostics[visibleIndex]) + '\n';
                            ImGui::SetClipboardText(all.c_str());
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::SameLine(0.0F, 0.0F);
                    ImGui::SetCursorScreenPos(rowStart);
                    ImGui::Dummy({iconSize + 4.0F, ImGui::GetTextLineHeight()});
                    const ImVec2 iconMin = ImGui::GetItemRectMin();
                    EditorUI::drawIcon(
                        ImGui::GetWindowDrawList(), levelIcon(level),
                        {iconMin.x + 2.0F + iconSize * 0.5F, iconMin.y + ImGui::GetTextLineHeight() * 0.5F}, iconSize,
                        ImGui::GetColorU32(levelColor(level)));
                    ImGui::SameLine(0.0F, 6.0F);
                    ImGui::TextDisabled("%s", formatTime(entry.timestamp).c_str());
                    ImGui::SameLine(0.0F, 10.0F);
                    const std::string message = formatDiagnostic(entry);
                    if (level == LogLevel::Info)
                        ImGui::TextUnformatted(message.c_str());
                    else
                        ImGui::TextColored(levelColor(level), "%s", message.c_str());
                    if (rowHovered && ImGui::CalcTextSize(message.c_str()).x > ImGui::GetContentRegionAvail().x) {
                        ImGui::BeginTooltip();
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0F);
                        ImGui::TextUnformatted(message.c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                    }
                    ImGui::PopID();
                }
            }
            if (ImGui::IsWindowFocused() && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C) &&
                selectedEntry < diagnostics.size()) {
                ImGui::SetClipboardText(formatLine(diagnostics[selectedEntry]).c_str());
            }
            // Follow new entries only while the view is already at the bottom,
            // so reading older messages is not interrupted.
            if (autoScroll && visible.size() != lastVisibleCount &&
                ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - ImGui::GetTextLineHeightWithSpacing() * 2.0F)
                ImGui::SetScrollHereY(1.0F);
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
    ImGui::PopFont();
    lastVisibleCount = visible.size();
    ImGui::End();
}

} // namespace Editor
