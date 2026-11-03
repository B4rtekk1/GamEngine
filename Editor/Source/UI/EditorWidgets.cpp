#include "Editor/UI/EditorWidgets.h"

#include "Editor/EditorUi.h"
#include "Editor/UI/EditorTheme.h"
#include "imgui_internal.h"

#include <algorithm>
#include <string>

// NOLINTBEGIN(readability-magic-numbers)

namespace EditorUI {

bool primaryButton(const char* label, const ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_Button, colors().accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors().accentHover);
    ImGui::PushStyleColor(ImGuiCol_Text, colors().appBackground);
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return pressed;
}

bool dangerButton(const char* label, const ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_Button, withAlpha(colors().error, 0.18F));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, withAlpha(colors().error, 0.42F));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, withAlpha(colors().error, 0.6F));
    ImGui::PushStyleColor(ImGuiCol_Text, colors().error);
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

void panelHeader(const char* title, const char* detail) {
    ImGui::TextDisabled("%s", title);
    if (detail != nullptr && *detail != '\0') {
        const float detailWidth = ImGui::CalcTextSize(detail).x;
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - detailWidth));
        ImGui::TextDisabled("%s", detail);
    }
    ImGui::Separator();
}

bool searchBox(const char* id, const char* hint, char* value, const std::size_t capacity, const float width) {
    ImGui::SetNextItemWidth(width);
    const ImVec2 padding = ImGui::GetStyle().FramePadding;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {padding.x + 18.0F, padding.y});
    const bool changed = ImGui::InputTextWithHint(id, hint, value, capacity);
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    ImGui::PopStyleVar();
    drawSearchIcon(minimum, maximum);
    return changed;
}

void emptyState(const Icon icon, const char* title, const char* description) {
    const float available = ImGui::GetContentRegionAvail().x;
    const float baseX = ImGui::GetCursorPosX();
    const float iconSize = ImGui::GetFontSize() * 2.6F;
    ImGui::SetCursorPosX(baseX + (available - iconSize) * 0.5F);
    ImGui::Dummy({iconSize, iconSize});
    const ImVec2 iconMin = ImGui::GetItemRectMin();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 center{iconMin.x + iconSize * 0.5F, iconMin.y + iconSize * 0.5F};
    drawList->AddCircleFilled(center, iconSize * 0.62F, ImGui::GetColorU32(withAlpha(colors().accent, 0.08F)), 32);
    drawIcon(drawList, icon, center, iconSize * 0.62F, ImGui::GetColorU32(colors().accent));
    ImGui::Spacing();
    ImGui::SetCursorPosX(baseX + (available - ImGui::CalcTextSize(title).x) * 0.5F);
    ImGui::TextUnformatted(title);
    // Centre each wrapped line of the description inside the panel.
    const float wrapWidth = std::min(available - 16.0F, 300.0F);
    const float startX = baseX + (available - wrapWidth) * 0.5F;
    ImGui::PushStyleColor(ImGuiCol_Text, colors().textSecondary);
    const char* text = description;
    const char* const end = description + std::char_traits<char>::length(description);
    while (text < end) {
        const char* lineEnd = ImGui::GetFont()->CalcWordWrapPosition(ImGui::GetFontSize(), text, end, wrapWidth);
        if (lineEnd == text)
            lineEnd = end;
        const float lineWidth = ImGui::CalcTextSize(text, lineEnd).x;
        ImGui::SetCursorPosX(startX + (wrapWidth - lineWidth) * 0.5F);
        ImGui::TextUnformatted(text, lineEnd);
        text = lineEnd;
        while (text < end && *text == ' ')
            ++text;
    }
    ImGui::PopStyleColor();
}

bool collapsingSection(const char* label, const ImGuiTreeNodeFlags flags) {
    // Header colours are tuned for selected rows. Section bars of the
    // inspector should stay neutral so they never read as "selected".
    ImGui::PushStyleColor(ImGuiCol_Header, colors().surfaceRaised);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, colors().controlHover);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, colors().controlHover);
    const bool open = ImGui::CollapsingHeader(label, flags);
    ImGui::PopStyleColor(3);
    return open;
}

bool collapsingSectionWithAction(const char* label, const Icon actionIcon, const char* actionTooltip,
                                 bool& actionClicked, const ImGuiTreeNodeFlags flags) {
    // AllowOverlap lets the action button drawn on top of the header receive
    // the click instead of toggling the section.
    const bool open = collapsingSection(label, flags | ImGuiTreeNodeFlags_AllowOverlap);
    const ImVec2 headerMin = ImGui::GetItemRectMin();
    const ImVec2 headerMax = ImGui::GetItemRectMax();
    const float size = headerMax.y - headerMin.y;
    const ImVec2 resume = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos({headerMax.x - size - 2.0F, headerMin.y});
    ImGui::PushID(label);
    actionClicked = iconButton("##section-action", actionIcon, actionTooltip, {.size = {size, size}});
    ImGui::PopID();
    ImGui::SetCursorScreenPos(resume);
    return open;
}

void toolbarSeparator() {
    ImGui::SameLine(0.0F, ImGui::GetStyle().ItemSpacing.x);
    const ImVec2 position = ImGui::GetCursorScreenPos();
    const float height = ImGui::GetFrameHeight();
    ImGui::GetWindowDrawList()->AddLine({position.x, position.y + height * 0.2F},
                                        {position.x, position.y + height * 0.8F}, ImGui::GetColorU32(colors().border),
                                        1.0F);
    ImGui::Dummy({1.0F, height});
    ImGui::SameLine(0.0F, ImGui::GetStyle().ItemSpacing.x);
}

bool segmentedControl(const char* id, const std::span<const char* const> labels, int& selected) {
    const Palette& palette = colors();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float height = ImGui::GetFrameHeight();
    constexpr float inset = 2.0F;
    float width = inset * 2.0F;
    for (const char* label : labels)
        width += ImGui::CalcTextSize(label, nullptr, true).x + style.FramePadding.x * 2.0F;

    ImGui::PushID(id);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(origin, {origin.x + width, origin.y + height}, ImGui::GetColorU32(palette.control),
                            style.FrameRounding);
    bool changed = false;
    float x = origin.x + inset;
    for (int index = 0; index < static_cast<int>(labels.size()); ++index) {
        const char* label = labels[static_cast<std::size_t>(index)];
        const float segmentWidth = ImGui::CalcTextSize(label, nullptr, true).x + style.FramePadding.x * 2.0F;
        ImGui::SetCursorScreenPos({x, origin.y + inset});
        ImGui::PushID(index);
        if (ImGui::InvisibleButton("##segment", {segmentWidth, height - inset * 2.0F}) && selected != index) {
            selected = index;
            changed = true;
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const bool active = selected == index;
        if (active || hovered) {
            drawList->AddRectFilled(min, max, ImGui::GetColorU32(active ? palette.surfaceRaised : palette.controlHover),
                                    std::max(0.0F, style.FrameRounding - 1.0F));
        }
        if (active) {
            drawList->AddLine({min.x + 6.0F, max.y - 1.0F}, {max.x - 6.0F, max.y - 1.0F},
                              ImGui::GetColorU32(palette.accent), 2.0F);
        }
        const ImVec2 textSize = ImGui::CalcTextSize(label, nullptr, true);
        drawList->AddText({min.x + (segmentWidth - textSize.x) * 0.5F, min.y + (max.y - min.y - textSize.y) * 0.5F},
                          ImGui::GetColorU32(active ? palette.textPrimary : palette.textSecondary), label,
                          ImGui::FindRenderedTextEnd(label));
        x += segmentWidth;
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy({width, height});
    ImGui::PopID();
    return changed;
}

void keyChip(const char* key) {
    const Palette& palette = colors();
    const ImVec2 padding{6.0F, 1.0F};
    const ImVec2 textSize = ImGui::CalcTextSize(key);
    const ImVec2 size{textSize.x + padding.x * 2.0F, textSize.y + padding.y * 2.0F};
    ImGui::AlignTextToFramePadding();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    const ImVec2 max{origin.x + size.x, origin.y + size.y};
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled({origin.x, origin.y + 1.0F}, {max.x, max.y + 1.0F}, ImGui::GetColorU32(palette.titleBar),
                            4.0F);
    drawList->AddRectFilled(origin, max, ImGui::GetColorU32(palette.surfaceRaised), 4.0F);
    drawList->AddRect(origin, max, ImGui::GetColorU32(palette.border), 4.0F);
    drawList->AddText({origin.x + padding.x, origin.y + padding.y}, ImGui::GetColorU32(palette.textPrimary), key);
}

void badge(const char* text, const ImVec4 background, const ImVec4 foreground) {
    const ImVec2 padding{7.0F, 1.0F};
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size{textSize.x + padding.x * 2.0F, textSize.y + padding.y * 2.0F};
    ImGui::Dummy(size);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(origin, {origin.x + size.x, origin.y + size.y}, ImGui::GetColorU32(background),
                            size.y * 0.5F);
    drawList->AddText({origin.x + padding.x, origin.y + padding.y}, ImGui::GetColorU32(foreground), text);
}

} // namespace EditorUI

// NOLINTEND(readability-magic-numbers)
