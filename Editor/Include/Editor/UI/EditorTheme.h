#pragma once

#include "imgui.h"

namespace EditorUI {

/** Semantic colours shared by editor panels and widgets. */
struct Palette final {
    ImVec4 appBackground;
    ImVec4 titleBar;
    ImVec4 panel;
    ImVec4 surface;
    ImVec4 surfaceRaised;
    ImVec4 control;
    ImVec4 controlHover;
    ImVec4 border;
    ImVec4 textPrimary;
    ImVec4 textSecondary;
    ImVec4 accent;
    ImVec4 accentHover;
    ImVec4 success;
    ImVec4 warning;
    ImVec4 error;
};

[[nodiscard]] const Palette& colors();

} // namespace EditorUI
