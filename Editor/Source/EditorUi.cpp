#include "Editor/EditorUi.h"

#include "Editor/EditorConstants.h"

#include <cctype>

void drawPanelHeader(const char *title, const char *subtitle) {
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = ImGui::GetTextLineHeight() + ImGui::GetStyle().FramePadding.y * 2.0F + 6.0F;
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(cursor, {cursor.x + width, cursor.y + height},
                            ImGui::GetColorU32(ImVec4{0.070F, 0.086F, 0.112F, 1.0F}), 6.0F);
    drawList->AddRectFilled(cursor, {cursor.x + 4.0F, cursor.y + height},
                            ImGui::GetColorU32(ImVec4{0.18F, 0.86F, 0.84F, 1.0F}), 3.0F);
    drawList->AddLine({cursor.x + 4.0F, cursor.y + height - 1.0F},
                      {cursor.x + width - 8.0F, cursor.y + height - 1.0F},
                      ImGui::GetColorU32(ImVec4{0.16F, 0.23F, 0.29F, 0.75F}));
    ImGui::SetCursorScreenPos({cursor.x + 14.0F, cursor.y + ImGui::GetStyle().FramePadding.y + 3.0F});
    ImGui::PushStyleColor(ImGuiCol_Text, {0.93F, 0.97F, 0.99F, 1.0F});
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    if (subtitle != nullptr) {
        ImGui::SameLine(0.0F, 10.0F);
        ImGui::TextDisabled("%s", subtitle);
    }
    ImGui::SetCursorScreenPos({cursor.x, cursor.y + height + 9.0F});
    ImGui::Dummy({0.0F, 0.0F});
}

bool drawToolbarToggle(const char *label, const bool active) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, {0.06F, 0.48F, 0.59F, 1.0F});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.10F, 0.62F, 0.70F, 1.0F});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.05F, 0.38F, 0.48F, 1.0F});
    }
    const bool pressed = ImGui::SmallButton(label);
    if (active) ImGui::PopStyleColor(3);
    return pressed;
}

void drawSearchIcon(const ImVec2 min, const ImVec2 max) {
    const ImVec2 center{min.x + 12.0F, (min.y + max.y) * 0.5F - 1.0F};
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    const ImU32 color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    drawList->AddCircle(center, 4.5F, color, EditorConstants::orientationSegmentCount, 1.6F);
    drawList->AddLine({center.x + 3.2F, center.y + 3.2F},
                      {center.x + EditorConstants::seven, center.y + EditorConstants::seven}, color, 1.6F);
}

bool containsCaseInsensitive(const char *text, const char *query) {
    if (*query == '\0') return true;
    for (; *text != '\0'; ++text) {
        const char *textIt = text;
        const char *queryIt = query;
        while (*textIt != '\0' && *queryIt != '\0' &&
               std::tolower(static_cast<unsigned char>(*textIt)) ==
               std::tolower(static_cast<unsigned char>(*queryIt))) {
            ++textIt;
            ++queryIt;
        }
        if (*queryIt == '\0') return true;
    }
    return false;
}
