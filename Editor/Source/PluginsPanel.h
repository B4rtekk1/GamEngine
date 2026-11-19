#pragma once

#include "Editor/UI/EditorIcons.h"
#include "Editor/UI/EditorTheme.h"
#include "Editor/UI/EditorWidgets.h"
#include "Engine/Plugins/PluginManager.h"
#include "Engine/Project.h"
#include "imgui.h"

#include <SDL3/SDL.h>
#include <filesystem>
#include <functional>
#include <string>

namespace Editor {
/**
 * Lists the plugins found beside the Editor and in the project, with their
 * state, and turns them on or off for the project (GamEngine.project's
 * disabled_plugins). Changes apply on the next reload.
 */
inline void drawPluginsPanel(bool& isOpen, const Engine::PluginManager& plugins, Engine::Project& project,
                             const std::filesystem::path& editorPluginRoot, const bool playing, const bool building,
                             const std::function<void()>& reload, const std::function<void(std::string)>& error) {
    if (!isOpen)
        return;
    if (!ImGui::Begin("Plugins", &isOpen)) {
        ImGui::End();
        return;
    }
    const EditorUI::Palette& palette = EditorUI::colors();
    const auto projectPluginRoot = project.rootPath() / "Plugins";
    static bool pendingChanges = false;

    ImGui::BeginDisabled(playing || building);
    if (ImGui::Button(building ? "Building plugins..." : "Reload plugins")) {
        reload();
        pendingChanges = false;
    }
    ImGui::EndDisabled();
    if (playing && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Stop Play Mode to reload plugins.");
    else if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Builds changed project plugins from their Source/ folders, then reloads all plugins.");
    ImGui::SameLine();
    if (ImGui::Button("Open project plugins folder")) {
        std::error_code ignored;
        std::filesystem::create_directories(projectPluginRoot, ignored);
        if (!SDL_OpenURL(("file://" + projectPluginRoot.string()).c_str()))
            error(std::string{"Could not open the plugins folder: "} + SDL_GetError());
    }
    if (pendingChanges) {
        ImGui::SameLine();
        ImGui::TextColored(palette.warning, "Reload to apply changes.");
    }
    ImGui::Separator();

    const auto& statuses = plugins.plugins();
    if (statuses.empty()) {
        EditorUI::emptyState(EditorUI::Icon::Archive, "No plugins installed",
                             "Put a plugin folder with its .geplugin manifest in the project's Plugins folder.");
        ImGui::End();
        return;
    }
    if (ImGui::BeginTable("##plugins", 5,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableSetupColumn("Plugin", ImGuiTableColumnFlags_WidthStretch, 0.45F);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch, 0.12F);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 0.28F);
        ImGui::TableSetupColumn("Provides", ImGuiTableColumnFlags_WidthStretch, 0.15F);
        ImGui::TableHeadersRow();
        for (const Engine::PluginStatus& status : statuses) {
            const Engine::PluginDescriptor& descriptor = status.descriptor;
            ImGui::TableNextRow();
            ImGui::PushID(descriptor.manifestPath.string().c_str());

            ImGui::TableSetColumnIndex(0);
            bool enabled = project.pluginEnabled(descriptor.name);
            if (ImGui::Checkbox("##enabled", &enabled)) {
                project.setPluginEnabled(descriptor.name, enabled);
                try {
                    project.save();
                    pendingChanges = true;
                } catch (const std::exception& exception) {
                    error(exception.what());
                }
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(descriptor.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", descriptor.version.c_str());
            if (!descriptor.description.empty())
                ImGui::TextDisabled("%s", descriptor.description.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", descriptor.directory.string().c_str());

            ImGui::TableSetColumnIndex(2);
            const auto inside = descriptor.directory.lexically_relative(editorPluginRoot);
            const bool engineWide = !inside.empty() && *inside.begin() != "..";
            ImGui::TextDisabled("%s", engineWide ? "Engine" : "Project");

            ImGui::TableSetColumnIndex(3);
            switch (status.state) {
            case Engine::PluginState::Loaded:
                ImGui::TextColored(palette.success, "Loaded");
                break;
            case Engine::PluginState::Disabled:
                ImGui::TextDisabled("Disabled");
                break;
            case Engine::PluginState::Discovered:
                ImGui::TextDisabled("Not loaded");
                break;
            case Engine::PluginState::Failed:
                if (building && std::filesystem::is_directory(descriptor.directory / "Source") &&
                    status.error.starts_with("No library")) {
                    ImGui::TextColored(palette.accent, "Building...");
                    break;
                }
                ImGui::TextColored(palette.error, "Failed");
                ImGui::PushTextWrapPos(0.0F);
                ImGui::TextDisabled("%s", status.error.c_str());
                ImGui::PopTextWrapPos();
                break;
            }

            ImGui::TableSetColumnIndex(4);
            if (status.state == Engine::PluginState::Loaded)
                ImGui::TextDisabled("%zu scripts, %zu systems", status.scriptCount, status.systemCount);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
} // namespace Editor
