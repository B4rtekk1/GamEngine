#pragma once

#include "imgui.h"

#include <utility>

namespace EditorUI {

/** A two-column inspector layout with stable label/value proportions. */
class PropertyGrid final {
public:
    explicit PropertyGrid(const char* id, float labelWidth = 0.38F);
    ~PropertyGrid();

    PropertyGrid(const PropertyGrid&) = delete;
    PropertyGrid& operator=(const PropertyGrid&) = delete;

    [[nodiscard]] bool active() const { return active_; }

    template <typename DrawValue>
    void propertyRow(const char* label, DrawValue&& drawValue) const {
        if (!active_) return;
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label);
        ImGui::TableSetColumnIndex(1);
        ImGui::PushID(label);
        ImGui::SetNextItemWidth(-1.0F);
        std::forward<DrawValue>(drawValue)();
        ImGui::PopID();
    }

private:
    bool active_{};
};

} // namespace EditorUI
