#include "Editor/UI/PropertyGrid.h"

namespace EditorUI {

PropertyGrid::PropertyGrid(const char* id, const float labelWidth) {
    active_ = ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings);
    if (active_) {
        ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthStretch, labelWidth);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0F - labelWidth);
    }
}

PropertyGrid::~PropertyGrid() {
    if (active_) ImGui::EndTable();
}

} // namespace EditorUI
