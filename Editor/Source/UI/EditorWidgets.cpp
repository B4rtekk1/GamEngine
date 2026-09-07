#include "Editor/UI/EditorWidgets.h"

#include "Editor/EditorUi.h"
#include "Editor/UI/EditorTheme.h"

#include <algorithm>

namespace EditorUI {

bool primaryButton(const char* label, const ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_Button, colors().accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors().accentHovered);
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(2);
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

bool searchBox(const char* id, const char* hint, char* value, const std::size_t capacity) {
    ImGui::SetNextItemWidth(-1.0F);
    const ImVec2 padding = ImGui::GetStyle().FramePadding;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {padding.x + 18.0F, padding.y});
    const bool changed = ImGui::InputTextWithHint(id, hint, value, capacity);
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    ImGui::PopStyleVar();
    drawSearchIcon(minimum, maximum);
    return changed;
}

void emptyState(const char* icon, const char* title, const char* description) {
    const float available = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleColor(ImGuiCol_Text, colors().accent);
    ImGui::SetCursorPosX((available - ImGui::CalcTextSize(icon).x) * 0.5F);
    ImGui::TextUnformatted(icon);
    ImGui::PopStyleColor();
    ImGui::SetCursorPosX((available - ImGui::CalcTextSize(title).x) * 0.5F);
    ImGui::TextDisabled("%s", title);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + available);
    ImGui::TextWrapped("%s", description);
    ImGui::PopTextWrapPos();
}

} // namespace EditorUI
