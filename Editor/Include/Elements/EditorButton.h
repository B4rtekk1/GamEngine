#pragma once

#include "imgui.h"

class EditorButton final {
public:
    explicit EditorButton(const char* label, const ImVec2 size = {}) noexcept
        : label_(label), size_(size) {}

    [[nodiscard]] bool draw() const {
        return ImGui::Button(label_, size_);
    }

    [[nodiscard]] bool drawSmall() const {
        return ImGui::SmallButton(label_);
    }

private:
    const char* label_;
    ImVec2 size_;
};
