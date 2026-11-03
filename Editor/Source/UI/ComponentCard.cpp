#include "Editor/UI/ComponentCard.h"

#include "Editor/UI/EditorTheme.h"
#include "Editor/UI/EditorWidgets.h"

namespace EditorUI {

ComponentCard::ComponentCard(const char* id, const char* name, const bool removable)
    : id_(id), name_(name), removable_(removable) {}

bool ComponentCard::begin() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, colors().surface);
    ImGui::PushStyleColor(ImGuiCol_Border, colors().border);
    // BeginChild owns its window/ID scope. Keeping the card identity in its
    // child ID avoids a second manual ID stack crossing that boundary.
    ImGui::BeginChild(id_, {0.0F, 0.0F}, ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    ImGui::PopStyleColor(2);

    if (removable_ && ImGui::BeginTable("component-card-header", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("title", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("actions", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        open_ = collapsingSection(name_);
        ImGui::TableSetColumnIndex(1);
        if (iconButton("##component-actions", Icon::Gear, "Component actions",
                       {.size = {ImGui::GetItemRectSize().y, ImGui::GetItemRectSize().y}}))
            ImGui::OpenPopup("component-actions");
        if (ImGui::BeginPopup("component-actions")) {
            ImGui::TextDisabled("%s", name_);
            ImGui::Separator();
            ImGui::MenuItem("Reset", nullptr, false, false);
            ImGui::MenuItem("Copy Component", nullptr, false, false);
            ImGui::MenuItem("Paste Component Values", nullptr, false, false);
            if (ImGui::MenuItem("Remove Component"))
                removeRequested_ = true;
            ImGui::EndPopup();
        }
        ImGui::EndTable();
    } else {
        open_ = collapsingSection(name_);
    }
    return open_;
}

void ComponentCard::end() {
    ImGui::EndChild();
    ImGui::Spacing();
}

} // namespace EditorUI
