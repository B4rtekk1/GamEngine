/**
 * @file EditorStyle.cpp
 * @brief Implements the visual theme and default docking layout of the editor.
 */

#include "Editor/Panels/EditorStyle.h"

#include "imgui.h"
#include "imgui_internal.h"

namespace {
    /** @name Editor geometry and spacing
     *  Constants controlling padding, spacing, border thickness and rounding.
     *  @{
     */
    constexpr float kWindowPaddingX = 16.0F;     ///< Horizontal window padding, in pixels.
    constexpr float kWindowPaddingY = 14.0F;     ///< Vertical window padding, in pixels.
    constexpr float kFramePaddingX = 10.0F;      ///< Horizontal frame padding, in pixels.
    constexpr float kFramePaddingY = 8.0F;       ///< Vertical frame padding, in pixels.
    constexpr float kItemSpacingX = 10.0F;       ///< Horizontal spacing between widgets, in pixels.
    constexpr float kItemSpacingY = 10.0F;       ///< Vertical spacing between widgets, in pixels.
    constexpr float kItemInnerSpacingX = 8.0F;   ///< Horizontal spacing inside compound widgets, in pixels.
    constexpr float kItemInnerSpacingY = 6.0F;   ///< Vertical spacing inside compound widgets, in pixels.
    constexpr float kScrollbarSize = 13.0F;      ///< Scrollbar width, in pixels.
    constexpr float kGrabMinSize = 12.0F;        ///< Minimum slider and scrollbar grab size, in pixels.
    constexpr float kBorderSize = 1.0F;          ///< Window and child border thickness, in pixels.
    constexpr float kFrameBorderSize = 0.0F;     ///< Frame border thickness, in pixels.
    constexpr float kWindowRounding = 8.0F;      ///< Window corner radius, in pixels.
    constexpr float kChildRounding = 7.0F;       ///< Child-window corner radius, in pixels.
    constexpr float kFrameRounding = 6.0F;       ///< Widget frame corner radius, in pixels.
    constexpr float kPopupRounding = 7.0F;       ///< Popup corner radius, in pixels.
    constexpr float kScrollbarRounding = 8.0F;   ///< Scrollbar corner radius, in pixels.
    constexpr float kGrabRounding = 6.0F;        ///< Slider and scrollbar grab corner radius, in pixels.
    constexpr float kTabRounding = 6.0F;         ///< Tab corner radius, in pixels.
    /** @} */

    /** @name Default docking layout
     *  Constants used to construct the initial editor workspace.
     *  @{
     */
    constexpr ImGuiID kEmptyDockNodeId = 0;       ///< Initial value for dock-node identifiers.
    constexpr float kHierarchyWidthRatio = 0.22F; ///< Fraction of the viewport assigned to the hierarchy panel.
    constexpr float kInspectorWidthRatio = 0.28F; ///< Fraction of the remaining center area assigned to the inspector.
    /** @} */
}

/**
 * @brief Applies the editor's global ImGui visual style.
 *
 * Resets ImGui to its dark theme and then configures editor-specific spacing,
 * border sizes, corner rounding and colors for windows, widgets, tabs, docking
 * previews and resize grips.
 *
 * @note The function modifies the global ImGuiStyle instance returned by
 *       ImGui::GetStyle().
 */
void EditorStyle::apply() {
    constexpr ImVec4 kWindowBackground = {0.075F, 0.086F, 0.110F, 1.0F};
    constexpr ImVec4 kChildBackground = {0.060F, 0.070F, 0.092F, 1.0F};
    constexpr ImVec4 kPopupBackground = {0.105F, 0.120F, 0.150F, 0.99F};
    constexpr ImVec4 kMenuBarBackground = {0.047F, 0.055F, 0.074F, 1.0F};
    constexpr ImVec4 kTitleBackground = {0.062F, 0.073F, 0.096F, 1.0F};
    constexpr ImVec4 kActiveTitleBackground = {0.092F, 0.128F, 0.150F, 1.0F};
    constexpr ImVec4 kHeader = {0.12F, 0.35F, 0.40F, 0.65F};
    constexpr ImVec4 kHoveredHeader = {0.13F, 0.55F, 0.62F, 0.62F};
    constexpr ImVec4 kActiveHeader = {0.10F, 0.68F, 0.75F, 0.82F};
    constexpr ImVec4 kButton = {0.115F, 0.155F, 0.205F, 1.0F};
    constexpr ImVec4 kHoveredButton = {0.15F, 0.43F, 0.50F, 1.0F};
    constexpr ImVec4 kActiveButton = {0.11F, 0.61F, 0.68F, 1.0F};
    constexpr ImVec4 kFrameBackground = {0.105F, 0.125F, 0.160F, 1.0F};
    constexpr ImVec4 kHoveredFrameBackground = {0.14F, 0.215F, 0.260F, 1.0F};
    constexpr ImVec4 kActiveFrameBackground = {0.12F, 0.33F, 0.38F, 1.0F};
    constexpr ImVec4 kBorder = {0.18F, 0.225F, 0.275F, 1.0F};
    constexpr ImVec4 kSeparator = {0.18F, 0.245F, 0.290F, 1.0F};
    constexpr ImVec4 kText = {0.91F, 0.94F, 0.98F, 1.0F};
    constexpr ImVec4 kDisabledText = {0.55F, 0.62F, 0.70F, 1.0F};
    constexpr ImVec4 kCheckMark = {0.30F, 0.90F, 0.86F, 1.0F};
    constexpr ImVec4 kSliderGrab = {0.20F, 0.72F, 0.76F, 1.0F};
    constexpr ImVec4 kActiveSliderGrab = {0.38F, 0.94F, 0.89F, 1.0F};
    constexpr ImVec4 kTab = {0.080F, 0.105F, 0.140F, 1.0F};
    constexpr ImVec4 kHoveredTab = {0.14F, 0.50F, 0.57F, 1.0F};
    constexpr ImVec4 kActiveTab = {0.12F, 0.31F, 0.37F, 1.0F};
    constexpr ImVec4 kDockingPreview = {0.20F, 0.82F, 0.86F, 0.55F};
    constexpr ImVec4 kResizeGrip = {0.18F, 0.65F, 0.70F, 0.30F};
    constexpr ImVec4 kHoveredResizeGrip = {0.28F, 0.84F, 0.85F, 0.75F};
    constexpr ImVec4 kActiveResizeGrip = {0.38F, 0.94F, 0.89F, 0.95F};

    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    // Generous spacing and distinct interaction states make dense editor
    // panels easier to scan and more comfortable to use for long sessions.
    style.WindowPadding = {kWindowPaddingX, kWindowPaddingY};
    style.FramePadding = {kFramePaddingX, kFramePaddingY};
    style.ItemSpacing = {kItemSpacingX, kItemSpacingY};
    style.ItemInnerSpacing = {kItemInnerSpacingX, kItemInnerSpacingY};
    style.ScrollbarSize = kScrollbarSize;
    style.GrabMinSize = kGrabMinSize;
    style.WindowBorderSize = kBorderSize;
    style.ChildBorderSize = kBorderSize;
    style.FrameBorderSize = kFrameBorderSize;
    style.WindowRounding = kWindowRounding;
    style.ChildRounding = kChildRounding;
    style.FrameRounding = kFrameRounding;
    style.PopupRounding = kPopupRounding;
    style.ScrollbarRounding = kScrollbarRounding;
    style.GrabRounding = kGrabRounding;
    style.TabRounding = kTabRounding;

    ImVec4 *colors = style.Colors;
    colors[ImGuiCol_WindowBg] = kWindowBackground;
    colors[ImGuiCol_ChildBg] = kChildBackground;
    colors[ImGuiCol_PopupBg] = kPopupBackground;
    colors[ImGuiCol_MenuBarBg] = kMenuBarBackground;
    colors[ImGuiCol_TitleBg] = kTitleBackground;
    colors[ImGuiCol_TitleBgActive] = kActiveTitleBackground;
    colors[ImGuiCol_TitleBgCollapsed] = kMenuBarBackground;
    colors[ImGuiCol_Header] = kHeader;
    colors[ImGuiCol_HeaderHovered] = kHoveredHeader;
    colors[ImGuiCol_HeaderActive] = kActiveHeader;
    colors[ImGuiCol_Button] = kButton;
    colors[ImGuiCol_ButtonHovered] = kHoveredButton;
    colors[ImGuiCol_ButtonActive] = kActiveButton;
    colors[ImGuiCol_FrameBg] = kFrameBackground;
    colors[ImGuiCol_FrameBgHovered] = kHoveredFrameBackground;
    colors[ImGuiCol_FrameBgActive] = kActiveFrameBackground;
    colors[ImGuiCol_Border] = kBorder;
    colors[ImGuiCol_Separator] = kSeparator;
    colors[ImGuiCol_Text] = kText;
    colors[ImGuiCol_TextDisabled] = kDisabledText;
    colors[ImGuiCol_CheckMark] = kCheckMark;
    colors[ImGuiCol_SliderGrab] = kSliderGrab;
    colors[ImGuiCol_SliderGrabActive] = kActiveSliderGrab;
    colors[ImGuiCol_Tab] = kTab;
    colors[ImGuiCol_TabHovered] = kHoveredTab;
    colors[ImGuiCol_TabActive] = kActiveTab;
    colors[ImGuiCol_DockingPreview] = kDockingPreview;
    colors[ImGuiCol_DockingEmptyBg] = kMenuBarBackground;
    colors[ImGuiCol_ResizeGrip] = kResizeGrip;
    colors[ImGuiCol_ResizeGripHovered] = kHoveredResizeGrip;
    colors[ImGuiCol_ResizeGripActive] = kActiveResizeGrip;
}

/**
 * @brief Creates the default editor docking layout once.
 *
 * Rebuilds the main viewport dock space and assigns the Hierarchy panel to the
 * left, the Viewport to the center and the Inspector to the right. Subsequent
 * calls return immediately so that user changes to the docking layout are not
 * overwritten during later frames.
 *
 * @pre An active ImGui context and main viewport must exist.
 *
 * @note The layout is configured once per process by means of a function-local
 *       static flag.
 */
void EditorStyle::configureDockLayout() {
    static bool configured = false;
    if (configured) {
        return;
    }
    const ImGuiID root = ImGui::GetMainViewport()->ID;
    ImGui::DockBuilderRemoveNode(root);
    ImGui::DockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(root, ImGui::GetMainViewport()->WorkSize);
    ImGuiID hierarchy = kEmptyDockNodeId;
    ImGuiID center = kEmptyDockNodeId;
    ImGui::DockBuilderSplitNode(root, ImGuiDir_Left, kHierarchyWidthRatio, &hierarchy, &center);
    ImGuiID inspector = kEmptyDockNodeId;
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, kInspectorWidthRatio, &inspector, &center);
    ImGui::DockBuilderDockWindow("Hierarchy", hierarchy);
    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderDockWindow("Inspector", inspector);
    ImGui::DockBuilderFinish(root);
    configured = true;
}
