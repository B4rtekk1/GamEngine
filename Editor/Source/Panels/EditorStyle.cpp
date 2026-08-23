#include "Editor/Panels/EditorStyle.h"

#include "imgui.h"
#include "imgui_internal.h"

void EditorStyle::apply() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    // Generous spacing and distinct interaction states make dense editor
    // panels easier to scan and more comfortable to use for long sessions.
    style.WindowPadding = {16.0f, 14.0f};
    style.FramePadding = {10.0f, 8.0f};
    style.ItemSpacing = {10.0f, 10.0f};
    style.ItemInnerSpacing = {8.0f, 6.0f};
    style.ScrollbarSize = 13.0f;
    style.GrabMinSize = 12.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowRounding = 8.0f;
    style.ChildRounding = 7.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 7.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding = 6.0f;
    style.TabRounding = 6.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = {0.075f, 0.086f, 0.110f, 1.0f};
    colors[ImGuiCol_ChildBg] = {0.060f, 0.070f, 0.092f, 1.0f};
    colors[ImGuiCol_PopupBg] = {0.105f, 0.120f, 0.150f, 0.99f};
    colors[ImGuiCol_MenuBarBg] = {0.047f, 0.055f, 0.074f, 1.0f};
    colors[ImGuiCol_TitleBg] = {0.062f, 0.073f, 0.096f, 1.0f};
    colors[ImGuiCol_TitleBgActive] = {0.092f, 0.128f, 0.150f, 1.0f};
    colors[ImGuiCol_TitleBgCollapsed] = {0.047f, 0.055f, 0.074f, 1.0f};
    colors[ImGuiCol_Header] = {0.12f, 0.35f, 0.40f, 0.65f};
    colors[ImGuiCol_HeaderHovered] = {0.13f, 0.55f, 0.62f, 0.62f};
    colors[ImGuiCol_HeaderActive] = {0.10f, 0.68f, 0.75f, 0.82f};
    colors[ImGuiCol_Button] = {0.115f, 0.155f, 0.205f, 1.0f};
    colors[ImGuiCol_ButtonHovered] = {0.15f, 0.43f, 0.50f, 1.0f};
    colors[ImGuiCol_ButtonActive] = {0.11f, 0.61f, 0.68f, 1.0f};
    colors[ImGuiCol_FrameBg] = {0.105f, 0.125f, 0.160f, 1.0f};
    colors[ImGuiCol_FrameBgHovered] = {0.14f, 0.215f, 0.260f, 1.0f};
    colors[ImGuiCol_FrameBgActive] = {0.12f, 0.33f, 0.38f, 1.0f};
    colors[ImGuiCol_Border] = {0.18f, 0.225f, 0.275f, 1.0f};
    colors[ImGuiCol_Separator] = {0.18f, 0.245f, 0.290f, 1.0f};
    colors[ImGuiCol_Text] = {0.91f, 0.94f, 0.98f, 1.0f};
    colors[ImGuiCol_TextDisabled] = {0.55f, 0.62f, 0.70f, 1.0f};
    colors[ImGuiCol_CheckMark] = {0.30f, 0.90f, 0.86f, 1.0f};
    colors[ImGuiCol_SliderGrab] = {0.20f, 0.72f, 0.76f, 1.0f};
    colors[ImGuiCol_SliderGrabActive] = {0.38f, 0.94f, 0.89f, 1.0f};
    colors[ImGuiCol_Tab] = {0.080f, 0.105f, 0.140f, 1.0f};
    colors[ImGuiCol_TabHovered] = {0.14f, 0.50f, 0.57f, 1.0f};
    colors[ImGuiCol_TabActive] = {0.12f, 0.31f, 0.37f, 1.0f};
    colors[ImGuiCol_DockingPreview] = {0.20f, 0.82f, 0.86f, 0.55f};
    colors[ImGuiCol_DockingEmptyBg] = {0.047f, 0.055f, 0.074f, 1.0f};
    colors[ImGuiCol_ResizeGrip] = {0.18f, 0.65f, 0.70f, 0.30f};
    colors[ImGuiCol_ResizeGripHovered] = {0.28f, 0.84f, 0.85f, 0.75f};
    colors[ImGuiCol_ResizeGripActive] = {0.38f, 0.94f, 0.89f, 0.95f};
}

void EditorStyle::configureDockLayout() {
    static bool configured = false;
    if (configured) return;
    const ImGuiID root = ImGui::GetMainViewport()->ID;
    ImGui::DockBuilderRemoveNode(root);
    ImGui::DockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(root, ImGui::GetMainViewport()->WorkSize);
    ImGuiID hierarchy = 0, center = 0;
    ImGui::DockBuilderSplitNode(root, ImGuiDir_Left, 0.22f, &hierarchy, &center);
    ImGuiID inspector = 0;
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, &inspector, &center);
    ImGui::DockBuilderDockWindow("Hierarchy", hierarchy);
    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderDockWindow("Inspector", inspector);
    ImGui::DockBuilderFinish(root);
    configured = true;
}
