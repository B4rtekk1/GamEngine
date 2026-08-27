#include "Editor/EditorUi.h"

#include "Editor/EditorConstants.h"

#include <cctype>

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
