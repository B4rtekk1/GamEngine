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
    constexpr float kWindowPaddingX = 12.0F;
    constexpr float kWindowPaddingY = 10.0F;
    constexpr float kFramePaddingX = 10.0F;
    constexpr float kFramePaddingY = 6.0F;
    constexpr float kItemSpacingX = 8.0F;
    constexpr float kItemSpacingY = 7.0F;
    constexpr float kItemInnerSpacingX = 8.0F;
    constexpr float kItemInnerSpacingY = 5.0F;
    constexpr float kIndentSpacing = 18.0F;
    constexpr float kScrollbarSize = 12.0F;
    constexpr float kGrabMinSize = 10.0F;
    constexpr float kBorderSize = 1.0F;
    constexpr float kFrameBorderSize = 0.0F;
    constexpr float kWindowRounding = 7.0F;
    constexpr float kChildRounding = 6.0F;
    constexpr float kFrameRounding = 6.0F;
    constexpr float kPopupRounding = 7.0F;
    constexpr float kScrollbarRounding = 8.0F;
    constexpr float kGrabRounding = 4.0F;
    constexpr float kTabRounding = 4.0F;
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
    constexpr ImVec4 kWindowBackground = {0.075F, 0.082F, 0.102F, 1.0F};
    constexpr ImVec4 kChildBackground = {0.055F, 0.061F, 0.078F, 1.0F};
    constexpr ImVec4 kPopupBackground = {0.105F, 0.116F, 0.145F, 0.99F};
    constexpr ImVec4 kMenuBarBackground = {0.047F, 0.052F, 0.067F, 1.0F};
    constexpr ImVec4 kTitleBackground = {0.062F, 0.069F, 0.088F, 1.0F};
    constexpr ImVec4 kActiveTitleBackground = {0.095F, 0.125F, 0.158F, 1.0F};
    constexpr ImVec4 kHeader = {0.075F, 0.345F, 0.460F, 0.52F};
    constexpr ImVec4 kHoveredHeader = {0.075F, 0.505F, 0.655F, 0.68F};
    constexpr ImVec4 kActiveHeader = {0.070F, 0.610F, 0.770F, 0.88F};
    constexpr ImVec4 kButton = {0.112F, 0.125F, 0.158F, 1.0F};
    constexpr ImVec4 kHoveredButton = {0.105F, 0.330F, 0.435F, 1.0F};
    constexpr ImVec4 kActiveButton = {0.070F, 0.500F, 0.640F, 1.0F};
    constexpr ImVec4 kFrameBackground = {0.092F, 0.103F, 0.132F, 1.0F};
    constexpr ImVec4 kHoveredFrameBackground = {0.115F, 0.145F, 0.185F, 1.0F};
    constexpr ImVec4 kActiveFrameBackground = {0.080F, 0.275F, 0.350F, 1.0F};
    constexpr ImVec4 kBorder = {0.175F, 0.205F, 0.250F, 0.82F};
    constexpr ImVec4 kSeparator = {0.150F, 0.180F, 0.225F, 1.0F};
    constexpr ImVec4 kText = {0.925F, 0.945F, 0.975F, 1.0F};
    constexpr ImVec4 kDisabledText = {0.500F, 0.560F, 0.650F, 1.0F};
    constexpr ImVec4 kCheckMark = {0.270F, 0.875F, 0.845F, 1.0F};
    constexpr ImVec4 kSliderGrab = {0.170F, 0.680F, 0.790F, 1.0F};
    constexpr ImVec4 kActiveSliderGrab = {0.300F, 0.920F, 0.900F, 1.0F};
    constexpr ImVec4 kTab = {0.060F, 0.067F, 0.085F, 1.0F};
    constexpr ImVec4 kHoveredTab = {0.085F, 0.310F, 0.405F, 1.0F};
    constexpr ImVec4 kActiveTab = {0.095F, 0.155F, 0.195F, 1.0F};
    constexpr ImVec4 kUnfocusedTab = {0.052F, 0.058F, 0.074F, 1.0F};
    constexpr ImVec4 kUnfocusedTabActive = {0.073F, 0.093F, 0.118F, 1.0F};
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
}

/**
 * @brief Creates the default editor docking layout once.
 */
void EditorStyle::configureDockLayout(const ImVec2 dockSize) {
    static bool configured = false;
    if (configured) {
        return;
    }
    const ImGuiID root = ImGui::GetMainViewport()->ID;
    ImGui::DockBuilderRemoveNode(root);
    ImGui::DockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(root, dockSize);
    ImGuiID assetManager = kEmptyDockNodeId;
    ImGuiID workspace = kEmptyDockNodeId;
    ImGui::DockBuilderSplitNode(root, ImGuiDir_Down, 0.30F, &assetManager, &workspace);
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
    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderDockWindow("Inspector", inspectorTop);
    ImGui::DockBuilderDockWindow("Terrain Tools", terrainTools);
    ImGui::DockBuilderFinish(root);
    configured = true;
}

// NOLINTEND(readability-magic-numbers)
