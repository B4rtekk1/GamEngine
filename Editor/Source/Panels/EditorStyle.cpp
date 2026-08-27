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
    constexpr float kWindowPaddingX = 14.0F;
    constexpr float kWindowPaddingY = 12.0F;
    constexpr float kFramePaddingX = 10.0F;
    constexpr float kFramePaddingY = 7.0F;
    constexpr float kItemSpacingX = 8.0F;
    constexpr float kItemSpacingY = 8.0F;
    constexpr float kItemInnerSpacingX = 8.0F;
    constexpr float kItemInnerSpacingY = 5.0F;
    constexpr float kIndentSpacing = 18.0F;
    constexpr float kScrollbarSize = 12.0F;
    constexpr float kGrabMinSize = 10.0F;
    constexpr float kBorderSize = 1.0F;
    constexpr float kFrameBorderSize = 0.0F;
    constexpr float kWindowRounding = 6.0F;
    constexpr float kChildRounding = 5.0F;
    constexpr float kFrameRounding = 5.0F;
    constexpr float kPopupRounding = 6.0F;
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
 * Soft charcoal surfaces with a calm sky accent keep long editing sessions
 * comfortable while selected controls remain easy to spot.
 */
void EditorStyle::apply() {
    // Warm charcoal surfaces — easier on the eyes than cold blue-black.
    constexpr ImVec4 kWindowBackground = {0.118F, 0.122F, 0.145F, 1.0F};
    constexpr ImVec4 kChildBackground = {0.098F, 0.102F, 0.122F, 1.0F};
    constexpr ImVec4 kPopupBackground = {0.145F, 0.150F, 0.175F, 0.98F};
    constexpr ImVec4 kMenuBarBackground = {0.086F, 0.090F, 0.110F, 1.0F};
    constexpr ImVec4 kTitleBackground = {0.098F, 0.102F, 0.122F, 1.0F};
    constexpr ImVec4 kActiveTitleBackground = {0.130F, 0.145F, 0.180F, 1.0F};
    // Soft sky accent for selection and interactive feedback.
    constexpr ImVec4 kHeader = {0.22F, 0.42F, 0.58F, 0.55F};
    constexpr ImVec4 kHoveredHeader = {0.28F, 0.52F, 0.70F, 0.70F};
    constexpr ImVec4 kActiveHeader = {0.32F, 0.58F, 0.78F, 0.90F};
    constexpr ImVec4 kButton = {0.165F, 0.175F, 0.210F, 1.0F};
    constexpr ImVec4 kHoveredButton = {0.26F, 0.48F, 0.66F, 1.0F};
    constexpr ImVec4 kActiveButton = {0.30F, 0.56F, 0.76F, 1.0F};
    constexpr ImVec4 kFrameBackground = {0.145F, 0.152F, 0.185F, 1.0F};
    constexpr ImVec4 kHoveredFrameBackground = {0.175F, 0.195F, 0.240F, 1.0F};
    constexpr ImVec4 kActiveFrameBackground = {0.200F, 0.280F, 0.360F, 1.0F};
    constexpr ImVec4 kBorder = {0.220F, 0.235F, 0.280F, 0.85F};
    constexpr ImVec4 kSeparator = {0.210F, 0.225F, 0.270F, 1.0F};
    constexpr ImVec4 kText = {0.93F, 0.94F, 0.96F, 1.0F};
    constexpr ImVec4 kDisabledText = {0.58F, 0.62F, 0.70F, 1.0F};
    constexpr ImVec4 kCheckMark = {0.55F, 0.82F, 1.0F, 1.0F};
    constexpr ImVec4 kSliderGrab = {0.42F, 0.68F, 0.90F, 1.0F};
    constexpr ImVec4 kActiveSliderGrab = {0.55F, 0.80F, 1.0F, 1.0F};
    constexpr ImVec4 kTab = {0.110F, 0.115F, 0.140F, 1.0F};
    constexpr ImVec4 kHoveredTab = {0.24F, 0.42F, 0.58F, 1.0F};
    constexpr ImVec4 kActiveTab = {0.165F, 0.195F, 0.255F, 1.0F};
    constexpr ImVec4 kUnfocusedTab = {0.100F, 0.105F, 0.128F, 1.0F};
    constexpr ImVec4 kUnfocusedTabActive = {0.130F, 0.140F, 0.175F, 1.0F};
    constexpr ImVec4 kDockingPreview = {0.40F, 0.70F, 0.95F, 0.45F};
    constexpr ImVec4 kResizeGrip = {0.35F, 0.55F, 0.75F, 0.25F};
    constexpr ImVec4 kHoveredResizeGrip = {0.45F, 0.70F, 0.92F, 0.70F};
    constexpr ImVec4 kActiveResizeGrip = {0.55F, 0.80F, 1.0F, 0.95F};
    constexpr ImVec4 kScrollbarBg = {0.080F, 0.084F, 0.100F, 0.60F};
    constexpr ImVec4 kScrollbarGrab = {0.28F, 0.32F, 0.40F, 1.0F};
    constexpr ImVec4 kScrollbarGrabHovered = {0.38F, 0.48F, 0.62F, 1.0F};
    constexpr ImVec4 kScrollbarGrabActive = {0.45F, 0.60F, 0.78F, 1.0F};

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
    ImGui::DockBuilderDockWindow("Hierarchy", hierarchy);
    ImGui::DockBuilderDockWindow("Asset Manager", assetManager);
    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderDockWindow("Inspector", inspector);
    ImGui::DockBuilderFinish(root);
    configured = true;
}
