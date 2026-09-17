/**
 * @file EditorStyle.cpp
 * @brief Implements the visual theme and default docking layout of the editor.
 */

#include "Editor/Panels/EditorStyle.h"
#include "Editor/UI/EditorTheme.h"

#include "imgui.h"
#include "imgui_internal.h"

// NOLINTBEGIN(readability-magic-numbers)

namespace {
    /** @name Editor geometry and spacing
     *  Constants controlling padding, spacing, border thickness and rounding.
     *  @{
     */
    constexpr float kWindowPaddingX = 8.0F;
    constexpr float kWindowPaddingY = 7.0F;
    constexpr float kFramePaddingX = 7.0F;
    constexpr float kFramePaddingY = 5.0F;
    constexpr float kItemSpacingX = 6.0F;
    constexpr float kItemSpacingY = 5.0F;
    constexpr float kItemInnerSpacingX = 5.0F;
    constexpr float kItemInnerSpacingY = 4.0F;
    constexpr float kIndentSpacing = 18.0F;
    constexpr float kScrollbarSize = 11.0F;
    constexpr float kGrabMinSize = 10.0F;
    constexpr float kBorderSize = 1.0F;
    constexpr float kFrameBorderSize = 0.0F;
    constexpr float kWindowRounding = 3.0F;
    constexpr float kChildRounding = 3.0F;
    constexpr float kFrameRounding = 3.0F;
    constexpr float kPopupRounding = 3.0F;
    constexpr float kScrollbarRounding = 3.0F;
    constexpr float kGrabRounding = 3.0F;
    constexpr float kTabRounding = 3.0F;
    /** @} */

    /** @name Default docking layout
     *  @{
     */
    constexpr ImGuiID kEmptyDockNodeId = 0;
    constexpr float kHierarchyWidthRatio = 0.22F;
    constexpr float kInspectorWidthRatio = 0.28F;
    /** @} */
}

/**
 * @brief Applies the editor's global ImGui visual style.
 *
 * Layered graphite surfaces and a cyan accent make the editor easier to scan
 * during long sessions, while keeping selected controls immediately obvious.
 */
void EditorStyle::apply() {
    const EditorUI::Palette& palette = EditorUI::colors();
    // Four deliberately distinct surface levels keep docked regions legible.
    // A cool, blue-black palette avoids the flat, uniformly-grey look of
    // the default ImGui theme. Surfaces are deliberately close together;
    // borders and the azure accent do the work of separating information.
    const ImVec4 kWindowBackground = palette.appBackground;
    const ImVec4 kChildBackground = palette.panel;
    const ImVec4 kPopupBackground = palette.surfaceRaised;
    const ImVec4 kMenuBarBackground = palette.titleBar;
    const ImVec4 kTitleBackground = palette.titleBar;
    const ImVec4 kActiveTitleBackground = palette.surface;
    const ImVec4 kHeader = palette.surfaceRaised;
    const ImVec4 kHoveredHeader = palette.controlHover;
    const ImVec4 kActiveHeader = palette.accent;
    const ImVec4 kButton = palette.control;
    const ImVec4 kHoveredButton = palette.controlHover;
    const ImVec4 kActiveButton = palette.accent;
    const ImVec4 kFrameBackground = palette.control;
    const ImVec4 kHoveredFrameBackground = palette.controlHover;
    const ImVec4 kActiveFrameBackground = palette.surfaceRaised;
    const ImVec4 kBorder = palette.border;
    const ImVec4 kSeparator = palette.border;
    const ImVec4 kText = palette.textPrimary;
    const ImVec4 kDisabledText = palette.textSecondary;
    const ImVec4 kCheckMark = palette.success;
    const ImVec4 kSliderGrab = palette.accent;
    const ImVec4 kActiveSliderGrab = palette.accentHover;
    const ImVec4 kTab = palette.panel;
    const ImVec4 kHoveredTab = palette.controlHover;
    const ImVec4 kActiveTab = palette.surfaceRaised;
    const ImVec4 kUnfocusedTab = palette.titleBar;
    const ImVec4 kUnfocusedTabActive = palette.panel;
    const ImVec4 kDockingPreview = {palette.accent.x, palette.accent.y, palette.accent.z, 0.42F};
    const ImVec4 kResizeGrip = {palette.accent.x, palette.accent.y, palette.accent.z, 0.22F};
    const ImVec4 kHoveredResizeGrip = {palette.accentHover.x, palette.accentHover.y, palette.accentHover.z, 0.66F};
    const ImVec4 kActiveResizeGrip = {palette.accentHover.x, palette.accentHover.y, palette.accentHover.z, 0.94F};
    const ImVec4 kScrollbarBg = {palette.appBackground.x, palette.appBackground.y, palette.appBackground.z, 0.72F};
    const ImVec4 kScrollbarGrab = palette.control;
    const ImVec4 kScrollbarGrabHovered = palette.controlHover;
    const ImVec4 kScrollbarGrabActive = palette.accent;

    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowPadding = {kWindowPaddingX, kWindowPaddingY};
    style.FramePadding = {kFramePaddingX, kFramePaddingY};
    style.ItemSpacing = {kItemSpacingX, kItemSpacingY};
    style.ItemInnerSpacing = {kItemInnerSpacingX, kItemInnerSpacingY};
    style.IndentSpacing = kIndentSpacing;
    style.ScrollbarSize = kScrollbarSize;
    style.GrabMinSize = kGrabMinSize;
    style.WindowBorderSize = kBorderSize;
    style.ChildBorderSize = kBorderSize;
    style.PopupBorderSize = kBorderSize;
    style.FrameBorderSize = kFrameBorderSize;
    style.WindowRounding = kWindowRounding;
    style.ChildRounding = kChildRounding;
    style.FrameRounding = kFrameRounding;
    style.PopupRounding = kPopupRounding;
    style.ScrollbarRounding = kScrollbarRounding;
    style.GrabRounding = kGrabRounding;
    style.TabRounding = kTabRounding;
    style.TabBorderSize = 0.0F;
    style.TabBarBorderSize = 1.0F;
    style.WindowMenuButtonPosition = ImGuiDir_Right;
    style.WindowTitleAlign = {0.0F, 0.5F};
    style.ButtonTextAlign = {0.5F, 0.5F};
    style.SelectableTextAlign = {0.0F, 0.5F};

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
    colors[ImGuiCol_BorderShadow] = {0.0F, 0.0F, 0.0F, 0.0F};
    colors[ImGuiCol_Separator] = kSeparator;
    colors[ImGuiCol_SeparatorHovered] = kHoveredHeader;
    colors[ImGuiCol_SeparatorActive] = kActiveHeader;
    colors[ImGuiCol_Text] = kText;
    colors[ImGuiCol_TextDisabled] = kDisabledText;
    colors[ImGuiCol_CheckMark] = kCheckMark;
    colors[ImGuiCol_SliderGrab] = kSliderGrab;
    colors[ImGuiCol_SliderGrabActive] = kActiveSliderGrab;
    colors[ImGuiCol_Tab] = kTab;
    colors[ImGuiCol_TabHovered] = kHoveredTab;
    colors[ImGuiCol_TabActive] = kActiveTab;
    colors[ImGuiCol_TabUnfocused] = kUnfocusedTab;
    colors[ImGuiCol_TabUnfocusedActive] = kUnfocusedTabActive;
    colors[ImGuiCol_DockingPreview] = kDockingPreview;
    colors[ImGuiCol_DockingEmptyBg] = kMenuBarBackground;
    colors[ImGuiCol_ResizeGrip] = kResizeGrip;
    colors[ImGuiCol_ResizeGripHovered] = kHoveredResizeGrip;
    colors[ImGuiCol_ResizeGripActive] = kActiveResizeGrip;
    colors[ImGuiCol_ScrollbarBg] = kScrollbarBg;
    colors[ImGuiCol_ScrollbarGrab] = kScrollbarGrab;
    colors[ImGuiCol_ScrollbarGrabHovered] = kScrollbarGrabHovered;
    colors[ImGuiCol_ScrollbarGrabActive] = kScrollbarGrabActive;
    colors[ImGuiCol_PlotHistogram] = kSliderGrab;
    colors[ImGuiCol_PlotLines] = kCheckMark;
    colors[ImGuiCol_NavHighlight] = kActiveHeader;
    colors[ImGuiCol_ModalWindowDimBg] = {palette.appBackground.x, palette.appBackground.y, palette.appBackground.z, 0.55F};
    colors[ImGuiCol_TableHeaderBg] = palette.surfaceRaised;
    colors[ImGuiCol_TableBorderStrong] = palette.border;
    colors[ImGuiCol_TableBorderLight] = palette.border;
    colors[ImGuiCol_TableRowBg] = {0.0F, 0.0F, 0.0F, 0.0F};
    colors[ImGuiCol_TableRowBgAlt] = {palette.surfaceRaised.x, palette.surfaceRaised.y, palette.surfaceRaised.z, 0.20F};
}

/**
 * @brief Creates the default editor docking layout once.
 */
void EditorStyle::configureDockLayout(const ImVec2 dockSize, const bool restorePersistedLayout) {
    static bool configured = false;
    if (configured) {
        return;
    }
    // ImGui applies IniFilename on its first frame. Do not subsequently erase
    // the restored dock nodes with the engine's first-run default layout.
    if (restorePersistedLayout) {
        configured = true;
        return;
    }
    const ImGuiID root = ImGui::GetMainViewport()->ID;
    ImGui::DockBuilderRemoveNode(root);
    ImGui::DockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(root, dockSize);
    ImGuiID bottom = kEmptyDockNodeId;
    ImGuiID workspace = kEmptyDockNodeId;
    ImGui::DockBuilderSplitNode(root, ImGuiDir_Down, 0.30F, &bottom, &workspace);
    ImGuiID assetManager = kEmptyDockNodeId;
    ImGuiID console = kEmptyDockNodeId;
    ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Right, 0.52F, &console, &assetManager);
    ImGuiID terminal = kEmptyDockNodeId;
    ImGui::DockBuilderSplitNode(console, ImGuiDir_Down, 0.50F, &terminal, &console);
    ImGuiID hierarchy = kEmptyDockNodeId;
    ImGuiID center = kEmptyDockNodeId;
    ImGui::DockBuilderSplitNode(workspace, ImGuiDir_Left, kHierarchyWidthRatio, &hierarchy, &center);
    ImGuiID inspector = kEmptyDockNodeId;
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, kInspectorWidthRatio, &inspector, &center);
    ImGuiID terrainTools = kEmptyDockNodeId;
    ImGuiID inspectorTop = kEmptyDockNodeId;
    ImGui::DockBuilderSplitNode(inspector, ImGuiDir_Down, 0.52F, &terrainTools, &inspectorTop);
    ImGui::DockBuilderDockWindow("Hierarchy", hierarchy);
    ImGui::DockBuilderDockWindow("Asset Manager", assetManager);
    ImGui::DockBuilderDockWindow("Console", console);
    ImGui::DockBuilderDockWindow("Terminal", terminal);
    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderDockWindow("Inspector", inspectorTop);
    ImGui::DockBuilderDockWindow("Terrain Tools", terrainTools);
    ImGui::DockBuilderFinish(root);
    configured = true;
}

// NOLINTEND(readability-magic-numbers)
