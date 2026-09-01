/**
 * @file EditorStyle.cpp
 * @brief Implements the visual theme and default docking layout of the editor.
 */

#include "Editor/Panels/EditorStyle.h"

#include "imgui.h"
#include "imgui_internal.h"

// NOLINTBEGIN(readability-magic-numbers)

namespace {
    /** @name Editor geometry and spacing
     *  Constants controlling padding, spacing, border thickness and rounding.
     *  @{
     */
    constexpr float kWindowPaddingX = 14.0F;
    constexpr float kWindowPaddingY = 12.0F;
    constexpr float kFramePaddingX = 10.0F;
    constexpr float kFramePaddingY = 7.0F;
    constexpr float kItemSpacingX = 9.0F;
    constexpr float kItemSpacingY = 8.0F;
    constexpr float kItemInnerSpacingX = 8.0F;
    constexpr float kItemInnerSpacingY = 6.0F;
    constexpr float kIndentSpacing = 18.0F;
    constexpr float kScrollbarSize = 11.0F;
    constexpr float kGrabMinSize = 10.0F;
    constexpr float kBorderSize = 1.0F;
    constexpr float kFrameBorderSize = 0.0F;
    constexpr float kWindowRounding = 8.0F;
    constexpr float kChildRounding = 7.0F;
    constexpr float kFrameRounding = 7.0F;
    constexpr float kPopupRounding = 8.0F;
    constexpr float kScrollbarRounding = 8.0F;
    constexpr float kGrabRounding = 4.0F;
    constexpr float kTabRounding = 6.0F;
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
    // Four deliberately distinct surface levels keep docked regions legible.
    // A cool, blue-black palette avoids the flat, uniformly-grey look of
    // the default ImGui theme. Surfaces are deliberately close together;
    // borders and the azure accent do the work of separating information.
    constexpr ImVec4 kWindowBackground = {0.060F, 0.070F, 0.095F, 1.0F};
    constexpr ImVec4 kChildBackground = {0.044F, 0.052F, 0.074F, 1.0F};
    constexpr ImVec4 kPopupBackground = {0.095F, 0.112F, 0.150F, 0.99F};
    constexpr ImVec4 kMenuBarBackground = {0.035F, 0.043F, 0.062F, 1.0F};
    constexpr ImVec4 kTitleBackground = {0.050F, 0.060F, 0.084F, 1.0F};
    constexpr ImVec4 kActiveTitleBackground = {0.075F, 0.142F, 0.190F, 1.0F};
    constexpr ImVec4 kHeader = {0.105F, 0.390F, 0.555F, 0.42F};
    constexpr ImVec4 kHoveredHeader = {0.110F, 0.530F, 0.720F, 0.62F};
    constexpr ImVec4 kActiveHeader = {0.105F, 0.640F, 0.835F, 0.82F};
    constexpr ImVec4 kButton = {0.100F, 0.120F, 0.165F, 1.0F};
    constexpr ImVec4 kHoveredButton = {0.110F, 0.355F, 0.500F, 1.0F};
    constexpr ImVec4 kActiveButton = {0.075F, 0.535F, 0.700F, 1.0F};
    constexpr ImVec4 kFrameBackground = {0.078F, 0.092F, 0.128F, 1.0F};
    constexpr ImVec4 kHoveredFrameBackground = {0.110F, 0.148F, 0.202F, 1.0F};
    constexpr ImVec4 kActiveFrameBackground = {0.080F, 0.290F, 0.390F, 1.0F};
    constexpr ImVec4 kBorder = {0.170F, 0.215F, 0.290F, 0.78F};
    constexpr ImVec4 kSeparator = {0.125F, 0.170F, 0.235F, 1.0F};
    constexpr ImVec4 kText = {0.910F, 0.940F, 0.990F, 1.0F};
    constexpr ImVec4 kDisabledText = {0.470F, 0.555F, 0.680F, 1.0F};
    constexpr ImVec4 kCheckMark = {0.330F, 0.900F, 0.840F, 1.0F};
    constexpr ImVec4 kSliderGrab = {0.200F, 0.705F, 0.850F, 1.0F};
    constexpr ImVec4 kActiveSliderGrab = {0.400F, 0.960F, 0.930F, 1.0F};
    constexpr ImVec4 kTab = {0.048F, 0.057F, 0.079F, 1.0F};
    constexpr ImVec4 kHoveredTab = {0.090F, 0.320F, 0.455F, 1.0F};
    constexpr ImVec4 kActiveTab = {0.095F, 0.185F, 0.250F, 1.0F};
    constexpr ImVec4 kUnfocusedTab = {0.040F, 0.047F, 0.066F, 1.0F};
    constexpr ImVec4 kUnfocusedTabActive = {0.065F, 0.095F, 0.135F, 1.0F};
    constexpr ImVec4 kDockingPreview = {0.120F, 0.820F, 0.900F, 0.42F};
    constexpr ImVec4 kResizeGrip = {0.200F, 0.640F, 0.750F, 0.22F};
    constexpr ImVec4 kHoveredResizeGrip = {0.220F, 0.800F, 0.880F, 0.66F};
    constexpr ImVec4 kActiveResizeGrip = {0.340F, 0.940F, 0.920F, 0.94F};
    constexpr ImVec4 kScrollbarBg = {0.035F, 0.040F, 0.052F, 0.72F};
    constexpr ImVec4 kScrollbarGrab = {0.205F, 0.255F, 0.325F, 1.0F};
    constexpr ImVec4 kScrollbarGrabHovered = {0.240F, 0.475F, 0.570F, 1.0F};
    constexpr ImVec4 kScrollbarGrabActive = {0.250F, 0.680F, 0.740F, 1.0F};

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
    colors[ImGuiCol_ModalWindowDimBg] = {0.04F, 0.045F, 0.06F, 0.55F};
    colors[ImGuiCol_TableHeaderBg] = {0.075F, 0.120F, 0.175F, 1.0F};
    colors[ImGuiCol_TableBorderStrong] = {0.140F, 0.190F, 0.260F, 0.82F};
    colors[ImGuiCol_TableBorderLight] = {0.095F, 0.135F, 0.190F, 0.64F};
    colors[ImGuiCol_TableRowBg] = {0.0F, 0.0F, 0.0F, 0.0F};
    colors[ImGuiCol_TableRowBgAlt] = {0.090F, 0.115F, 0.155F, 0.20F};
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
    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderDockWindow("Inspector", inspectorTop);
    ImGui::DockBuilderDockWindow("Terrain Tools", terrainTools);
    ImGui::DockBuilderFinish(root);
    configured = true;
}

// NOLINTEND(readability-magic-numbers)
