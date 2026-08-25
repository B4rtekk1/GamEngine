#include "Editor/Panels/EditorStyle.h"

#include "imgui.h"
#include "imgui_internal.h"

void EditorStyle::apply() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    // Generous spacing and distinct interaction states make dense editor
    // panels easier to scan and more comfortable to use for long sessions.
    style.WindowPadding = {16.0F, 14.0F};
    style.FramePadding = {10.0F, 8.0F};
    style.ItemSpacing = {10.0F, 10.0F};
    style.ItemInnerSpacing = {8.0F, 6.0F};
    style.ScrollbarSize = 13.0F;
    style.GrabMinSize = 12.0F;
    style.WindowBorderSize = 1.0F;
    style.ChildBorderSize = 1.0F;
    style.FrameBorderSize = 0.0F;
    style.WindowRounding = 8.0F;
    style.ChildRounding = 7.0F;
    style.FrameRounding = 6.0F;
    style.PopupRounding = 7.0F;
    style.ScrollbarRounding = 8.0F;
    style.GrabRounding = 6.0F;
    style.TabRounding = 6.0F;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = {0.075F, 0.086F, 0.110F, 1.0F};
    colors[ImGuiCol_ChildBg] = {0.060F, 0.070F, 0.092F, 1.0F};
    colors[ImGuiCol_PopupBg] = {0.105F, 0.120F, 0.150F, 0.99F};
    colors[ImGuiCol_MenuBarBg] = {0.047F, 0.055F, 0.074F, 1.0F};
    colors[ImGuiCol_TitleBg] = {0.062F, 0.073F, 0.096F, 1.0F};
    colors[ImGuiCol_TitleBgActive] = {0.092F, 0.128F, 0.150F, 1.0F};
    colors[ImGuiCol_TitleBgCollapsed] = {0.047F, 0.055F, 0.074F, 1.0F};
    colors[ImGuiCol_Header] = {0.12F, 0.35F, 0.40F, 0.65F};
    colors[ImGuiCol_HeaderHovered] = {0.13F, 0.55F, 0.62F, 0.62F};
    colors[ImGuiCol_HeaderActive] = {0.10F, 0.68F, 0.75F, 0.82F};
    colors[ImGuiCol_Button] = {0.115F, 0.155F, 0.205F, 1.0F};
    colors[ImGuiCol_ButtonHovered] = {0.15F, 0.43F, 0.50F, 1.0F};
    colors[ImGuiCol_ButtonActive] = {0.11F, 0.61F, 0.68F, 1.0F};
    colors[ImGuiCol_FrameBg] = {0.105F, 0.125F, 0.160F, 1.0F};
    colors[ImGuiCol_FrameBgHovered] = {0.14F, 0.215F, 0.260F, 1.0F};
    colors[ImGuiCol_FrameBgActive] = {0.12F, 0.33F, 0.38F, 1.0F};
    colors[ImGuiCol_Border] = {0.18F, 0.225F, 0.275F, 1.0F};
    colors[ImGuiCol_Separator] = {0.18F, 0.245F, 0.290F, 1.0F};
    colors[ImGuiCol_Text] = {0.91F, 0.94F, 0.98F, 1.0F};
    colors[ImGuiCol_TextDisabled] = {0.55F, 0.62F, 0.70F, 1.0F};
    colors[ImGuiCol_CheckMark] = {0.30F, 0.90F, 0.86F, 1.0F};
    colors[ImGuiCol_SliderGrab] = {0.20F, 0.72F, 0.76F, 1.0F};
    colors[ImGuiCol_SliderGrabActive] = {0.38F, 0.94F, 0.89F, 1.0F};
    colors[ImGuiCol_Tab] = {0.080F, 0.105F, 0.140F, 1.0F};
    colors[ImGuiCol_TabHovered] = {0.14F, 0.50F, 0.57F, 1.0F};
    colors[ImGuiCol_TabActive] = {0.12F, 0.31F, 0.37F, 1.0F};
    colors[ImGuiCol_DockingPreview] = {0.20F, 0.82F, 0.86F, 0.55F};
    colors[ImGuiCol_DockingEmptyBg] = {0.047F, 0.055F, 0.074F, 1.0F};
    colors[ImGuiCol_ResizeGrip] = {0.18F, 0.65F, 0.70F, 0.30F};
    colors[ImGuiCol_ResizeGripHovered] = {0.28F, 0.84F, 0.85F, 0.75F};
    colors[ImGuiCol_ResizeGripActive] = {0.38F, 0.94F, 0.89F, 0.95F};
}

void EditorStyle::configureDockLayout() {
    static bool configured = false;
    if (configured) return;
    const ImGuiID root = ImGui::GetMainViewport()->ID;
    ImGui::DockBuilderRemoveNode(root);
    ImGui::DockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(root, ImGui::GetMainViewport()->WorkSize);
    ImGuiID hierarchy = 0, center = 0;
    ImGui::DockBuilderSplitNode(root, ImGuiDir_Left, 0.22F, &hierarchy, &center);
    ImGuiID inspector = 0;
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28F, &inspector, &center);
    ImGui::DockBuilderDockWindow("Hierarchy", hierarchy);
    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderDockWindow("Inspector", inspector);
    ImGui::DockBuilderFinish(root);
    configured = true;
}