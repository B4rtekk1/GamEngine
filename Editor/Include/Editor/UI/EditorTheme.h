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
    /** Low-contrast accent used for selected rows and active toggles. */
    ImVec4 accentMuted;
    ImVec4 selection;
    ImVec4 selectionHover;
    /** Translucent background of controls drawn over the rendered viewport. */
    ImVec4 overlay;
    ImVec4 overlayBorder;
    ImVec4 success;
    ImVec4 warning;
    ImVec4 error;
    /** Entity-type tints used by hierarchy and inspector icons. */
    ImVec4 iconMesh;
    ImVec4 iconCamera;
    ImVec4 iconLight;
    ImVec4 iconTerrain;
    ImVec4 iconWater;
};

[[nodiscard]] const Palette& colors();

[[nodiscard]] constexpr ImVec4 withAlpha(const ImVec4 colour, const float alpha) {
    return {colour.x, colour.y, colour.z, alpha};
}

} // namespace EditorUI
