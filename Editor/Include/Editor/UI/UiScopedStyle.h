#pragma once

#include "imgui.h"

namespace EditorUI {

/** Balances a temporary ImGui colour override, including early returns. */
class ScopedColor final {
public:
    ScopedColor(const ImGuiCol colour, const ImVec4 value) { ImGui::PushStyleColor(colour, value); }
    ~ScopedColor() { ImGui::PopStyleColor(); }

    ScopedColor(const ScopedColor&) = delete;
    ScopedColor& operator=(const ScopedColor&) = delete;
};

} // namespace EditorUI
