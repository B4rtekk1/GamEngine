#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"

#include "Engine/Renderer/Vulkan/renderer.h"
#include "Engine/Scene/ScenePresets.h"
#include "Engine/Core/Time.h"
#include "Engine/Core/Transform.h"
#include "Engine/ECS/Components/ScriptComponent.h"
#include "Engine/ECS/Components/CameraComponent.h"
#include "Engine/ECS/Components/ColorPickerComponent.h"
#include "Engine/Renderer/MeshRenderer.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scripting/ScriptSystem.h"
#include "Elements/EditorButton.h"
#include "Elements/TransformFields.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace {

void configureEditorStyle() {
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

void drawPanelHeader(const char* title, const char* subtitle = nullptr) {
    ImGui::PushStyleColor(ImGuiCol_Text, {0.33f, 0.86f, 0.92f, 1.0f});
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    if (subtitle != nullptr) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", subtitle);
    }
    ImGui::Separator();
}

const char* entityName(const Engine::ScenePreset& scene, const Engine::Entity entity) {
    if (scene.registry.has<Engine::NameComponent>(entity)) {
        return scene.registry.get<Engine::NameComponent>(entity).value.c_str();
    }
    // A mesh can also be driven by a script. Keep the controller identity
    // visible in the hierarchy and inspector instead of hiding it behind the
    // generic GameObject label.
    if (scene.registry.has<Engine::ScriptComponent>(entity)) return "Controller";
    if (entity == scene.plane) return "Plane";
    if (entity == scene.camera) return "Camera";
    if (entity == scene.tree) return "Tree";
    for (const Engine::Entity editorCube : scene.editorCubes) {
        if (editorCube == entity) return "Cube";
    }
    for (const Engine::Entity editorPlane : scene.editorPlanes) {
        if (editorPlane == entity) return "Plane";
    }
    for (std::size_t index = 0; index < scene.editorGameObjects.size(); ++index) {
        if (scene.editorGameObjects[index] == entity) return "GameObject";
    }
    return "Entity";
}

std::filesystem::path editorScenePath() {
    return std::filesystem::path{GAMEENGINE_SOURCE_DIR} / "Assets" / "Scenes" / "Editor.scene";
}

bool setPlayMode(const bool play, Engine::ScenePreset& scene, std::string& snapshot,
                 std::string& error) {
    try {
        if (play) {
            std::ostringstream output;
            Engine::SceneSerializer::save(scene.registry, output);
            snapshot = std::move(output).str();
        } else {
            std::istringstream input{snapshot};
            Engine::SceneSerializer::load(scene.registry, input);
            snapshot.clear();
        }
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool createCppScript(const std::string_view name, std::string& error) {
    if (name.empty() || !std::isalpha(static_cast<unsigned char>(name.front())) ||
        !std::ranges::all_of(name, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; })) {
        error = "Use a valid C++ class name."; return false;
    }
    const auto dir = std::filesystem::path{GAMEENGINE_SOURCE_DIR} / "Sandbox/Source/Scripts";
    const auto header = dir / (std::string{name} + ".h");
    const auto source = dir / (std::string{name} + ".cpp");
    if (std::filesystem::exists(header) || std::filesystem::exists(source)) { error = "Script already exists."; return false; }
    std::filesystem::create_directories(dir);
    std::ofstream h{header}, cpp{source};
    if (!h || !cpp) { error = "Could not create script files."; return false; }
    h << "// Generated by GamEngine. You can safely edit this script.\n"
      << "#pragma once\n#include <Engine/Scripting/Script.h>\n\nclass " << name << " final : public Engine::Script {\npublic:\n    void onCreate() override {}\n    void onUpdate(float deltaTime) override { (void)deltaTime; }\n};\n";
    cpp << "// Generated by GamEngine. You can safely edit this script.\n"
        << "#include \"" << name << ".h\"\n#include <Engine/Scripting/ScriptRegistry.h>\n\nENGINE_REGISTER_SCRIPT(" << name << ");\n";
    return static_cast<bool>(h) && static_cast<bool>(cpp);
}

bool drawViewport(VkDescriptorSet gameDescriptor, VkDescriptorSet sceneDescriptor,
                  bool& showGameView, const bool playing) {
    if (playing) showGameView = true;
    const VkDescriptorSet descriptor = showGameView ? gameDescriptor : sceneDescriptor;

    ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoScrollbar);
    drawPanelHeader("VIEWPORT", playing ? "PLAYING  /  GAME CAMERA" :
                                  showGameView ? "GAME CAMERA" : "SCENE CAMERA");
    if (!playing) {
        ImGui::SameLine(ImGui::GetWindowWidth() - 154.0f);
    }
    if (!playing && EditorButton(showGameView ? "Scene View" : "Game View").draw()) {
        showGameView = !showGameView;
    }
    bool viewportHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const ImVec2 size = ImGui::GetContentRegionAvail();
    if (descriptor != VK_NULL_HANDLE && size.x > 1.0f && size.y > 1.0f) {
        constexpr float viewportAspect = 16.0f / 9.0f;
        // Keep the rendered view at a fixed aspect ratio. The child clips the
        // image when the panel is wider than 16:9, so the excess is removed
        // symmetrically from the top and bottom instead of distorting it.
        ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.018f, 0.024f, 0.034f, 1.0f});
        ImGui::BeginChild("##viewport-frame", size, true,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const ImVec2 frameSize = ImGui::GetContentRegionAvail();
        const float imageHeight = frameSize.x / viewportAspect;
        const float verticalOffset = (frameSize.y - imageHeight) * 0.5f;
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + verticalOffset);
        ImGui::Image(ImTextureRef{static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(descriptor))},
                     {frameSize.x, imageHeight}, {0, 0}, {1, 1});
        viewportHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    ImGui::End();
    // Do not enable camera navigation just because a mouse button is held
    // elsewhere in the editor. The previous global button check captured the
    // cursor after right- or middle-clicking menus and side panels, leaving
    // ImGui unable to receive subsequent clicks.
    return !playing && !showGameView && viewportHovered;
}

Engine::Entity drawHierarchy(Engine::ScenePreset& scene, const Engine::Entity selected) {
    Engine::Entity clicked = Engine::NullEntity;
    ImGui::Begin("Hierarchy");
    drawPanelHeader("SCENE HIERARCHY");
    ImGui::SameLine(ImGui::GetWindowWidth() - 75.0f);
    if (EditorButton("+").drawSmall()) clicked = scene.createGameObject();
    ImGui::SameLine(ImGui::GetWindowWidth() - 45.0f);
    ImGui::TextDisabled("%zu", scene.registry.size());
    static char filter[64] = {};
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##hierarchy-filter", "  Search objects...", filter, sizeof(filter));
    ImGui::Spacing();
    ImGui::TextDisabled("OBJECTS");

    if (ImGui::TreeNodeEx("Scene", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth)) {
        const auto entityLabel = [&](const char* name, const Engine::Entity entity) {
            if (entity != Engine::NullEntity) {
                if (filter[0] != '\0' && std::strstr(name, filter) == nullptr) return;
                char label[64];
                std::snprintf(label, sizeof(label), "%s  (%u)", name,
                              Engine::entityIndex(entity));
                if (ImGui::Selectable(label, selected == entity)) clicked = entity;
            }
        };

        // Parent links use persistent UUIDs, rather than recyclable ECS ids.
        // This makes the hierarchy survive save/load and entity reallocation.
        std::unordered_map<Engine::UUID, Engine::Entity> byUuid;
        std::vector<Engine::Entity> entities;
        entities.reserve(scene.registry.size());
        scene.registry.view<>([&](const Engine::Entity entity) {
            entities.push_back(entity);
            if (scene.registry.has<Engine::UUIDComponent>(entity)) {
                byUuid.emplace(scene.registry.get<Engine::UUIDComponent>(entity).value, entity);
            }
        });
        std::ranges::sort(entities);

        std::unordered_map<Engine::Entity, std::vector<Engine::Entity>> children;
        std::vector<Engine::Entity> roots;
        for (const Engine::Entity entity : entities) {
            if (scene.registry.has<Engine::ParentComponent>(entity)) {
                const Engine::UUID parent = scene.registry.get<Engine::ParentComponent>(entity).parent;
                if (const auto found = byUuid.find(parent); found != byUuid.end()) {
                    children[found->second].push_back(entity);
                    continue;
                }
            }
            roots.push_back(entity);
        }

        std::unordered_set<Engine::Entity> visited;
        const auto drawNode = [&](auto&& self, const Engine::Entity entity) -> void {
            if (!visited.insert(entity).second) return;
            const char* name = entityName(scene, entity);
            if (filter[0] != '\0' && std::strstr(name, filter) == nullptr) return;
            const auto childIt = children.find(entity);
            const bool hasChildren = childIt != children.end() && !childIt->second.empty();
            char label[128];
            std::snprintf(label, sizeof(label), "%s##%u", name, Engine::entityIndex(entity));
            if (!hasChildren) {
                if (ImGui::Selectable(label, selected == entity)) clicked = entity;
                return;
            }
            const ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth |
                (selected == entity ? ImGuiTreeNodeFlags_Selected : 0);
            const bool open = ImGui::TreeNodeEx(label, flags);
            if (ImGui::IsItemClicked()) clicked = entity;
            if (open) {
                for (const Engine::Entity child : childIt->second) self(self, child);
                ImGui::TreePop();
            }
        };
        for (const Engine::Entity entity : roots) drawNode(drawNode, entity);
        ImGui::TreePop();
    }

    ImGui::End();
    return clicked;
}

bool drawInspector(Engine::ScenePreset& scene, const Engine::Entity selected) {
    ImGui::Begin("Inspector");
    drawPanelHeader("INSPECTOR", selected == Engine::NullEntity ? "NO SELECTION" : "ENTITY");
    if (selected == Engine::NullEntity) {
        ImGui::Spacing();
        ImGui::TextColored({0.33f, 0.86f, 0.92f, 1.0f}, "  ◇");
        ImGui::SameLine();
        ImGui::TextDisabled("Nothing selected");
        ImGui::Spacing();
        ImGui::TextWrapped("Select an object in the Scene Hierarchy to inspect it.");
        const bool consumesMouseWheel = ImGui::IsWindowHovered(
            ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::GetIO().MouseWheel != 0.0f;
        ImGui::End();
        return consumesMouseWheel;
    }

    ImGui::TextColored({0.92f, 0.95f, 1.0f, 1.0f}, "%s", entityName(scene, selected));
    ImGui::SameLine();
    ImGui::TextDisabled("Entity %u", Engine::entityIndex(selected));
    if (scene.registry.valid(selected) && scene.registry.has<Engine::NameComponent>(selected)) {
        const Engine::Registry& readRegistry = scene.registry;
        const auto& name = readRegistry.get<Engine::NameComponent>(selected).value;
        char editableName[260]{};
        std::snprintf(editableName, sizeof(editableName), "%s", name.c_str());
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputTextWithHint("##object-name", "Object name", editableName, sizeof(editableName)) &&
            editableName[0] != '\0') {
            scene.registry.modify<Engine::NameComponent>(selected, [&](auto& value) {
                value.value = editableName;
            });
        }
    }
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen) &&
        scene.registry.valid(selected) && scene.registry.has<Engine::Transform>(selected)) {
        TransformFields{scene.registry, selected}.draw();
    }
    if (scene.registry.valid(selected) && scene.registry.has<Engine::ScriptComponent>(selected) &&
        ImGui::CollapsingHeader("Script", ImGuiTreeNodeFlags_DefaultOpen)) {
        const Engine::Registry& readRegistry = scene.registry;
        const Engine::ScriptComponent& script = readRegistry.get<Engine::ScriptComponent>(selected);
        char className[260]{};
        std::snprintf(className, sizeof(className), "%s", script.className.c_str());
        ImGui::TextDisabled("C++ script class");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText("##script-class", className, sizeof(className))) {
            scene.registry.modify<Engine::ScriptComponent>(selected, [&](auto& value) {
                value.className = className;
                value.reset();
            });
        }
        bool enabled = script.enabled;
        if (ImGui::Checkbox("Enabled##script", &enabled)) {
            scene.registry.modify<Engine::ScriptComponent>(selected, [&](auto& value) {
                value.enabled = enabled;
            });
        }
    }
    if (scene.registry.valid(selected) && scene.registry.has<Engine::ColorPickerComponent>(selected) &&
        ImGui::CollapsingHeader("Color Picker", ImGuiTreeNodeFlags_DefaultOpen)) {
        const Engine::Registry& readRegistry = scene.registry;
        const Engine::ColorPickerComponent& picker =
            readRegistry.get<Engine::ColorPickerComponent>(selected);
        float rgba[4] = {picker.color.r(), picker.color.g(), picker.color.b(), picker.color.a()};
        if (ImGui::ColorEdit4("Color", rgba, ImGuiColorEditFlags_AlphaBar)) {
            scene.registry.modify<Engine::ColorPickerComponent>(selected, [&](auto& component) {
                component.color = Engine::Color{rgba[0], rgba[1], rgba[2], rgba[3]};
            });
        }
    }
    ImGui::TextDisabled("COMPONENTS");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1.0f);
    if (EditorButton("New Component", {-1.0f, 0.0f}).draw()) {
        ImGui::OpenPopup("Add Component");
    }
    if (ImGui::BeginPopup("Add Component")) {
        const bool hasScript = scene.registry.has<Engine::ScriptComponent>(selected);
        const bool hasColorPicker = scene.registry.has<Engine::ColorPickerComponent>(selected);
        if (ImGui::MenuItem("Script", nullptr, false, !hasScript)) {
            scene.registry.add<Engine::ScriptComponent>(selected);
            ImGui::CloseCurrentPopup();
        }
        if (hasScript) ImGui::TextDisabled("Script component already added");
        if (ImGui::MenuItem("Color Picker", nullptr, false, !hasColorPicker)) {
            scene.registry.add<Engine::ColorPickerComponent>(selected);
            ImGui::CloseCurrentPopup();
        }
        if (hasColorPicker) ImGui::TextDisabled("Color Picker component already added");
        ImGui::EndPopup();
    }
    if (EditorButton("Attach C++ Script", {-1.0f, 0.0f}).draw()) ImGui::OpenPopup("Attach C++ Script");
    if (ImGui::BeginPopupModal("Attach C++ Script", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static char attachedClassName[128]{};
        ImGui::TextUnformatted("Enter the registered C++ script class name.");
        ImGui::InputTextWithHint("Class name", "CubeMovement", attachedClassName, sizeof(attachedClassName));
        const bool validName = attachedClassName[0] != '\0';
        if (EditorButton("Attach", {100.0f, 0.0f}).draw() && validName) {
            if (!scene.registry.has<Engine::ScriptComponent>(selected)) {
                scene.registry.add<Engine::ScriptComponent>(selected);
            }
            scene.registry.modify<Engine::ScriptComponent>(selected, [&](auto& script) {
                script.className = attachedClassName;
                script.enabled = true;
                script.reset();
            });
            attachedClassName[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (EditorButton("Cancel", {100.0f, 0.0f}).draw()) {
            attachedClassName[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (EditorButton("Create C++ Script", {-1.0f, 0.0f}).draw()) ImGui::OpenPopup("Create C++ Script");
    if (ImGui::BeginPopupModal("Create C++ Script", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static char name[128]{}; static std::string error;
        ImGui::TextUnformatted("Creates Sandbox/Source/Scripts/<Name>.h and .cpp");
        ImGui::InputTextWithHint("Class name", "PlayerController", name, sizeof(name));
        if (!error.empty()) ImGui::TextColored({1, .3f, .3f, 1}, "%s", error.c_str());
        if (EditorButton("Create").draw() && createCppScript(name, error)) {
            if (!scene.registry.has<Engine::ScriptComponent>(selected)) scene.registry.add<Engine::ScriptComponent>(selected);
            scene.registry.modify<Engine::ScriptComponent>(selected, [&](auto& script) { script.className = name; script.reset(); });
            name[0] = '\0'; error.clear(); ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine(); if (EditorButton("Cancel").draw()) { name[0] = '\0'; error.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    const bool consumesMouseWheel = ImGui::IsWindowHovered(
        ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::GetIO().MouseWheel != 0.0f;
    ImGui::End();
    return consumesMouseWheel;
}

void drawStatusBar(const Engine::ScenePreset& scene, const Engine::Entity selected,
                   const bool playing, const bool paused) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - 27.0f});
    ImGui::SetNextWindowSize({viewport->WorkSize.x, 27.0f});
    ImGui::Begin("##status-bar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav);
    ImGui::TextColored({0.25f, 0.80f, 0.87f, 1.0f}, "●");
    ImGui::SameLine();
    ImGui::TextUnformatted(playing ? (paused ? "Paused" : "Playing") : "Ready");
    ImGui::SameLine();
    ImGui::TextDisabled("|  Particle scene  |  Entities: %zu  |  Selected: %s",
                        scene.registry.size(), selected == Engine::NullEntity ? "None" : entityName(scene, selected));
    ImGui::End();
}

void configureEditorDockLayout() {
    static bool configured = false;
    if (configured) return;

    const ImGuiID dockspaceId = ImGui::GetMainViewport()->ID;
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

    // Keep the center node as the "remaining" node after each split. This
    // makes the intended layout explicit and guarantees that Viewport gets
    // all space left between the two side panels.
    ImGuiID hierarchyId = 0;
    ImGuiID centerId = 0;
    ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.22f,
                                &hierarchyId, &centerId);

    ImGuiID inspectorId = 0;
    ImGui::DockBuilderSplitNode(centerId, ImGuiDir_Right, 0.24f,
                                &inspectorId, &centerId);

    ImGui::DockBuilderDockWindow("Hierarchy", hierarchyId);
    ImGui::DockBuilderDockWindow("Viewport", centerId);
    ImGui::DockBuilderDockWindow("Inspector", inspectorId);
    ImGui::DockBuilderFinish(dockspaceId);
    configured = true;
}

Engine::Entity drawEditorMenuBar(Engine::ScenePreset& scene, Engine::Renderer& renderer,
                                 bool& antialiasingChanged, bool& sceneLoaded,
                                 const bool playing, const bool paused, bool& playToggleRequested,
                                 bool& pauseToggleRequested) {
    static bool showShortcuts = false;
    static bool showAbout = false;
    static bool openSceneSettings = false;
    static int antialiasingType = -1;
    static int msaaSamples = -1;
    static std::string sceneFileError;
    Engine::Entity createdEntity = Engine::NullEntity;

    if (!ImGui::BeginMainMenuBar()) return Engine::NullEntity;

    ImGui::PushStyleColor(ImGuiCol_Text, {0.34f, 0.87f, 0.93f, 1.0f});
    ImGui::TextUnformatted("GAMENGINE");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextDisabled("EDITOR");
    ImGui::SameLine();
    ImGui::TextDisabled("|");

    if (ImGui::BeginMenu("File")) {
        const std::filesystem::path scenePath = editorScenePath();
        if (ImGui::MenuItem("Save Scene", "Ctrl+S")) {
            try {
                std::filesystem::create_directories(scenePath.parent_path());
                Engine::SceneSerializer::save(scene.registry, scenePath);
                sceneFileError.clear();
            } catch (const std::exception& error) { sceneFileError = error.what(); }
        }
        if (ImGui::MenuItem("Load Scene", "Ctrl+O")) {
            try {
                Engine::SceneSerializer::load(scene.registry, scenePath);
                sceneLoaded = true;
                sceneFileError.clear();
            } catch (const std::exception& error) { sceneFileError = error.what(); }
        }
        ImGui::Separator();
        if (ImGui::MenuItem(playing ? "Stop" : "Play", "F5")) {
            playToggleRequested = true;
        }
        if (ImGui::MenuItem(paused ? "Resume" : "Pause", "F6", false, playing)) {
            pauseToggleRequested = true;
        }
        if (!sceneFileError.empty()) ImGui::TextDisabled("%s", sceneFileError.c_str());
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("GameObject")) {
        if (ImGui::MenuItem("Create Empty", "Ctrl+Shift+N")) {
            createdEntity = scene.createGameObject();
        }
        if (ImGui::MenuItem("Create Cube")) {
            createdEntity = scene.createCube();
        }
        if (ImGui::MenuItem("Create Plane")) {
            createdEntity = scene.createPlane();
        }
        ImGui::EndMenu();
    }

    // Keep the most common action visible even when the File menu is closed.
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, {0.08f, 0.42f, 0.29f, 1.0f});
    if (EditorButton(playing ? "  Stop  " : "  Play  ").draw()) {
        playToggleRequested = true;
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(playing ? "Stop play mode and restore the editor scene (F5)"
                                  : "Run the current scene in Game View (F5)");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!playing);
    if (EditorButton(paused ? "  Resume  " : "  Pause  ").draw()) {
        pauseToggleRequested = true;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Pause or resume script updates (F6)");
    }

    if (ImGui::BeginMenu("Scene")) {
        if (ImGui::MenuItem("Antialiasing...")) openSceneSettings = true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        // The editor does not yet have a command history, so keep these
        // commands visible and disabled until the corresponding systems exist.
        ImGui::MenuItem("Undo", "Ctrl+Z", false, false);
        ImGui::MenuItem("Redo", "Ctrl+Y", false, false);
        ImGui::Separator();
        ImGui::MenuItem("Cut", "Ctrl+X", false, false);
        ImGui::MenuItem("Copy", "Ctrl+C", false, false);
        ImGui::MenuItem("Paste", "Ctrl+V", false, false);
        ImGui::MenuItem("Duplicate", "Ctrl+D", false, false);
        ImGui::Separator();
        ImGui::MenuItem("Select All", "Ctrl+A", false, false);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Keyboard Shortcuts")) showShortcuts = true;
        ImGui::Separator();
        if (ImGui::MenuItem("About GamEngine Editor")) showAbout = true;
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();

    if (openSceneSettings) {
        if (antialiasingType < 0) {
            antialiasingType = renderer.antialiasingLevel() == Engine::AntialiasingLevel::Off ? 0 : 1;
            msaaSamples = renderer.antialiasingLevel() == Engine::AntialiasingLevel::MSAA2x ? 2 : 4;
        }
        ImGui::OpenPopup("Scene Settings");
        openSceneSettings = false;
    }

    if (ImGui::BeginPopupModal("Scene Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        constexpr const char* typeLabels[] = {"None", "MSAA", "FXAA (placeholder)", "TAA (placeholder)"};
        constexpr const char* sampleLabels[] = {"2x", "4x"};

        ImGui::TextUnformatted("Antialiasing");
        ImGui::Separator();
        ImGui::SetNextItemWidth(230.0f);
        if (ImGui::BeginCombo("Type", typeLabels[antialiasingType])) {
            for (int index = 0; index < 4; ++index) {
                const bool isSelected = antialiasingType == index;
                if (ImGui::Selectable(typeLabels[index], isSelected)) {
                    antialiasingType = index;
                    if (antialiasingType == 0) {
                        renderer.setAntialiasingLevel(Engine::AntialiasingLevel::Off);
                        antialiasingChanged = true;
                    } else if (antialiasingType == 1) {
                        renderer.setAntialiasingLevel(msaaSamples == 2
                            ? Engine::AntialiasingLevel::MSAA2x
                            : Engine::AntialiasingLevel::MSAA4x);
                        antialiasingChanged = true;
                    }
                }
                if (isSelected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::Spacing();
        if (antialiasingType == 1) {
            ImGui::SetNextItemWidth(230.0f);
            if (ImGui::BeginCombo("Samples", msaaSamples == 2 ? sampleLabels[0] : sampleLabels[1])) {
                for (const int samples : {2, 4}) {
                    const bool isSelected = msaaSamples == samples;
                    if (ImGui::Selectable(samples == 2 ? sampleLabels[0] : sampleLabels[1], isSelected)) {
                        msaaSamples = samples;
                        renderer.setAntialiasingLevel(samples == 2
                            ? Engine::AntialiasingLevel::MSAA2x
                            : Engine::AntialiasingLevel::MSAA4x);
                        antialiasingChanged = true;
                    }
                    if (isSelected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::TextDisabled("MSAA is currently supported by the renderer.");
        } else if (antialiasingType == 2 || antialiasingType == 3) {
            ImGui::BeginDisabled();
            float placeholderValue = 1.0f;
            ImGui::SliderFloat("Quality", &placeholderValue, 0.0f, 1.0f);
            ImGui::EndDisabled();
            ImGui::TextDisabled("Placeholder: this antialiasing type is not implemented yet.");
        } else {
            ImGui::TextDisabled("Antialiasing is disabled.");
        }

        ImGui::Separator();
        ImGui::TextDisabled("Changes are applied after reloading the scene.");
        if (EditorButton("Close").draw()) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (showShortcuts) {
        ImGui::Begin("Keyboard Shortcuts", &showShortcuts);
        ImGui::TextUnformatted("Editor shortcuts");
        ImGui::Separator();
        ImGui::BulletText("WASD + mouse: move and look in Scene View");
        ImGui::BulletText("Ctrl+D: duplicate selected object (coming soon)");
        ImGui::BulletText("Ctrl+Z / Ctrl+Y: undo / redo (coming soon)");
        ImGui::End();
    }

    if (showAbout) {
        ImGui::Begin("About GamEngine Editor", &showAbout);
        ImGui::TextUnformatted("GamEngine Editor");
        ImGui::TextUnformatted("A lightweight scene and particle editor.");
        ImGui::Separator();
        ImGui::TextUnformatted("Unity-inspired workspace");
        ImGui::End();
    }

    return createdEntity;
}

} // namespace

int main() {
    try {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) throw std::runtime_error(SDL_GetError());
        SDL_Window* window = SDL_CreateWindow("GamEngine Editor - Particles", 1280, 720,
                                              SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
        if (!window) throw std::runtime_error(SDL_GetError());

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
        configureEditorStyle();

        Engine::ScenePreset scene(Engine::SceneType::Particles);
        Engine::ScriptSystem scriptSystem{Engine::ScriptRegistry::instance()};
        Engine::Renderer renderer;
        renderer.initialize(scene, window);
        Engine::Entity selectedEntity = Engine::NullEntity;
        constexpr auto targetFrame = std::chrono::microseconds{16'667};
        bool running = true;
        bool rendererReloadPending = false;
        bool playing = false;
        bool paused = false;
        bool showGameView = false;
        std::string playSceneSnapshot;
        std::string playModeError;
        while (running) {
            const auto start = std::chrono::steady_clock::now();
            renderer.beginFrame();
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                // The editor owns Dear ImGui's platform event stream. Feed it
                // directly before the renderer handles engine input, so UI
                // controls never depend on renderer-internal state.
                ImGui_ImplSDL3_ProcessEvent(&event);
                renderer.processEvent(event);
                if (event.type == SDL_EVENT_QUIT ||
                    (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                     event.window.windowID == SDL_GetWindowID(window))) running = false;
                if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F5 &&
                    !ImGui::GetIO().WantTextInput) {
                    if (setPlayMode(!playing, scene, playSceneSnapshot, playModeError)) {
                        playing = !playing;
                        paused = false;
                        showGameView = playing;
                        rendererReloadPending = !playing;
                    }
                }
                if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F6 && playing &&
                    !ImGui::GetIO().WantTextInput) {
                    paused = !paused;
                }
            }
            if (!running) break;

            // Antialiasing changes recreate render-target resources. Ordinary
            // scene edits are synchronized below without rebuilding the UI or
            // swapchain.
            if (rendererReloadPending) {
                renderer.reloadScene(scene, window);
                rendererReloadPending = false;
            }

            const std::uint64_t sceneStructureBeforeUi = scene.registry.structuralRevision();
            renderer.beginEditorUiFrame();
            bool antialiasingChanged = false;
            bool sceneLoaded = false;
            bool playToggleRequested = false;
            bool pauseToggleRequested = false;
            if (const Engine::Entity created = drawEditorMenuBar(scene, renderer,
                                                                  antialiasingChanged, sceneLoaded,
                                                                  playing, paused, playToggleRequested,
                                                                  pauseToggleRequested);
                created != Engine::NullEntity) {
                selectedEntity = created;
                renderer.setEditorSelection(selectedEntity);
            }
            if (playToggleRequested && setPlayMode(!playing, scene, playSceneSnapshot, playModeError)) {
                playing = !playing;
                paused = false;
                showGameView = playing;
                if (!playing) {
                    selectedEntity = Engine::NullEntity;
                    renderer.setEditorSelection(selectedEntity);
                    rendererReloadPending = true;
                }
            }
            if (pauseToggleRequested && playing) paused = !paused;
            const ImGuiID dockspaceId = ImGui::GetMainViewport()->ID;
            ImGui::DockSpaceOverViewport(dockspaceId, ImGui::GetMainViewport(),
                                         ImGuiDockNodeFlags_PassthruCentralNode);
            configureEditorDockLayout();
            if (const Engine::Entity clicked = drawHierarchy(scene, selectedEntity);
                clicked != Engine::NullEntity) {
                selectedEntity = clicked;
                renderer.setEditorSelection(selectedEntity);
            }
            const bool sceneCameraInput = drawViewport(
                renderer.gameViewportDescriptor(), renderer.sceneViewportDescriptor(), showGameView, playing);
            const bool inspectorConsumesMouseWheel = drawInspector(scene, selectedEntity);
            drawStatusBar(scene, selectedEntity, playing, paused);
            renderer.setEditorSceneCameraInput(
                sceneCameraInput && !inspectorConsumesMouseWheel);
            ImGui::Render();

            if (antialiasingChanged) rendererReloadPending = true;
            if (sceneLoaded) {
                selectedEntity = Engine::NullEntity;
                renderer.setEditorSelection(selectedEntity);
                rendererReloadPending = true;
            }

            const bool sceneStructureChanged = !sceneLoaded &&
                scene.registry.structuralRevision() != sceneStructureBeforeUi;
            if (sceneStructureChanged) renderer.synchronizeScene(scene);
            if (playing && !paused) {
                scriptSystem.update(scene.registry, static_cast<float>(Engine::Time::deltaTime()));
            }
            renderer.renderFrame();

            const auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed < targetFrame) std::this_thread::sleep_for(targetFrame - elapsed);
        }
        renderer.shutdown();
        ImGui::DestroyContext();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Editor error: %s\n", error.what());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "GamEngine Editor error",
                                 error.what(), nullptr);
        return 1;
    }
}
