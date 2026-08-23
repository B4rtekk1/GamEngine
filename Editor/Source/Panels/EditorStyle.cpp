#include "Editor/Panels/EditorStyle.h"

#include "imgui.h"
#include "imgui_internal.h"

void EditorStyle::apply() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = {14.0f, 11.0f};
    style.FramePadding = {9.0f, 7.0f};
    style.ItemSpacing = {8.0f, 8.0f};
    style.ItemInnerSpacing = {6.0f, 5.0f};
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 10.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowRounding = 6.0f;
    style.ChildRounding = 5.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 5.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 4.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = {0.055f, 0.068f, 0.090f, 1.0f};
    colors[ImGuiCol_ChildBg] = {0.043f, 0.054f, 0.073f, 1.0f};
    colors[ImGuiCol_PopupBg] = {0.075f, 0.090f, 0.120f, 0.98f};
    colors[ImGuiCol_MenuBarBg] = {0.035f, 0.045f, 0.063f, 1.0f};
    colors[ImGuiCol_TitleBg] = {0.045f, 0.060f, 0.080f, 1.0f};
    colors[ImGuiCol_TitleBgActive] = {0.060f, 0.095f, 0.120f, 1.0f};
    colors[ImGuiCol_TitleBgCollapsed] = {0.035f, 0.045f, 0.063f, 1.0f};
    colors[ImGuiCol_Header] = {0.10f, 0.24f, 0.29f, 0.70f};
    colors[ImGuiCol_HeaderHovered] = {0.10f, 0.48f, 0.56f, 0.55f};
    colors[ImGuiCol_HeaderActive] = {0.08f, 0.62f, 0.70f, 0.75f};
    colors[ImGuiCol_Button] = {0.09f, 0.15f, 0.20f, 1.0f};
    colors[ImGuiCol_ButtonHovered] = {0.10f, 0.39f, 0.47f, 1.0f};
    colors[ImGuiCol_ButtonActive] = {0.08f, 0.55f, 0.63f, 1.0f};
    colors[ImGuiCol_FrameBg] = {0.08f, 0.11f, 0.15f, 1.0f};
    colors[ImGuiCol_FrameBgHovered] = {0.11f, 0.20f, 0.25f, 1.0f};
    colors[ImGuiCol_FrameBgActive] = {0.10f, 0.29f, 0.34f, 1.0f};
    colors[ImGuiCol_Border] = {0.13f, 0.19f, 0.24f, 1.0f};
    colors[ImGuiCol_Separator] = {0.13f, 0.20f, 0.25f, 1.0f};
    colors[ImGuiCol_Text] = {0.86f, 0.91f, 0.96f, 1.0f};
    colors[ImGuiCol_TextDisabled] = {0.45f, 0.53f, 0.61f, 1.0f};
    colors[ImGuiCol_CheckMark] = {0.20f, 0.82f, 0.90f, 1.0f};
    colors[ImGuiCol_SliderGrab] = {0.13f, 0.65f, 0.74f, 1.0f};
    colors[ImGuiCol_SliderGrabActive] = {0.25f, 0.87f, 0.93f, 1.0f};
    colors[ImGuiCol_Tab] = {0.07f, 0.12f, 0.16f, 1.0f};
    colors[ImGuiCol_TabHovered] = {0.10f, 0.45f, 0.53f, 1.0f};
    colors[ImGuiCol_TabActive] = {0.09f, 0.27f, 0.32f, 1.0f};
    colors[ImGuiCol_DockingPreview] = {0.12f, 0.70f, 0.80f, 0.50f};
    colors[ImGuiCol_DockingEmptyBg] = {0.035f, 0.045f, 0.063f, 1.0f};
    colors[ImGuiCol_ResizeGrip] = {0.12f, 0.55f, 0.64f, 0.25f};
    colors[ImGuiCol_ResizeGripHovered] = {0.20f, 0.78f, 0.86f, 0.70f};
    colors[ImGuiCol_ResizeGripActive] = {0.25f, 0.87f, 0.93f, 0.90f};
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
