#pragma once

#include "imgui.h"

namespace EditorUI {

/** Semantic colours shared by editor panels and widgets. */
struct Palette final {
    ImVec4 background;
    ImVec4 surface;
    ImVec4 surfaceRaised;
    ImVec4 text;
    ImVec4 textMuted;
    ImVec4 accent;
    ImVec4 accentHovered;
    ImVec4 success;
    ImVec4 warning;
    ImVec4 error;
    ImVec4 border;
};

[[nodiscard]] const Palette& colors();

} // namespace EditorUI
