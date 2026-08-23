#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"

#include "Engine/Renderer/Renderer.h"
#include "Engine/Scene/ScenePresets.h"
#include "Engine/Core/Time.h"
#include "Engine/Core/Transform.h"
#include "Engine/ECS/Components/ScriptComponent.h"
#include "Engine/ECS/Components/CameraComponent.h"
#include "Engine/ECS/Components/ColliderComponent.h"
#include "Engine/ECS/Components/RigidbodyComponent.h"
#include "Engine/ECS/Components/ColorPickerComponent.h"
#include "Engine/ECS/Components/SmokeEmitterComponent.h"
#include "Engine/Renderer/MeshRenderer.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scripting/ScriptSystem.h"
#include "Engine/Physics/PhysicsSystem.h"
#include "Elements/EditorButton.h"
#include "Elements/TransformFields.h"
#include "Editor/Panels/EditorSceneSession.h"
#include "Editor/Panels/EditorStyle.h"
#include "Editor/Panels/HierarchyPanel.h"
#include "Editor/Panels/ComponentsPanel.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <optional>
#include <unordered_map>
#include <unordered_set>

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
    if (scene.editor().has<Engine::NameComponent>(entity)) {
        return scene.editor().get<Engine::NameComponent>(entity).value.c_str();
    }
    // A mesh can also be driven by a script. Keep the controller identity
    // visible in the hierarchy and inspector instead of hiding it behind the
    // generic GameObject label.
    if (scene.editor().has<Engine::ScriptComponent>(entity)) return "Controller";
    if (entity == scene.plane) return "Plane";
    if (entity == scene.camera) return "Camera";
    if (entity == scene.tree) return "Tree";
    for (const Engine::Entity editorCube : scene.editorCubes) {
        if (editorCube == entity) return "Cube";
    }
    for (const Engine::Entity editorPlane : scene.editorPlanes) {
        if (editorPlane == entity) return "Plane";
    }
    for (const Engine::Entity editorSphere : scene.editorSpheres) {
        if (editorSphere == entity) return "Sphere";
    }
    for (const Engine::Entity editorRamp : scene.editorRamps) {
        if (editorRamp == entity) return "Ramp";
    }
    for (std::size_t index = 0; index < scene.editorGameObjects.size(); ++index) {
        if (scene.editorGameObjects[index] == entity) return "GameObject";
    }
    return "Entity";
}


void drawSceneOrientationGizmo(const ImVec2 imageMin, const ImVec2 imageMax,
                               const float yawDegrees, const float pitchDegrees) {
    constexpr float pi = 3.14159265358979323846f;
    constexpr float radius = 40.0f;
    constexpr float axisLength = 32.0f;
    const float yaw = yawDegrees * pi / 180.0f;
    const float pitch = pitchDegrees * pi / 180.0f;

    // Express world axes in the Scene View camera's screen-space basis.
    const float right[3]{-std::sin(yaw), 0.0f, std::cos(yaw)};
    const float up[3]{-std::cos(yaw) * std::sin(pitch), std::cos(pitch),
                      -std::sin(yaw) * std::sin(pitch)};
    const ImVec2 center{imageMax.x - radius - 12.0f, imageMax.y - radius - 12.0f};
    const ImU32 colors[3]{IM_COL32(255, 45, 45, 255), IM_COL32(36, 245, 79, 255),
                          IM_COL32(45, 135, 255, 255)};
    constexpr const char* labels[3]{"X", "Y", "Z"};

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    for (int axis = 0; axis < 3; ++axis) {
        const ImVec2 end{center.x + right[axis] * axisLength,
                         center.y - up[axis] * axisLength};
        drawList->AddLine(center, end, colors[axis], 4.0f);
        drawList->AddCircleFilled(end, 5.0f, colors[axis]);
        drawList->AddText({end.x + 5.0f, end.y - 7.0f}, colors[axis], labels[axis]);
    }
    drawList->AddCircleFilled(center, 4.0f, IM_COL32(255, 255, 255, 255));
}

bool drawViewport(Engine::ViewportHandle gameDescriptor, Engine::ViewportHandle sceneDescriptor,
                  const float sceneCameraYaw, const float sceneCameraPitch,
                  bool& showGameView, const bool playing) {
    if (playing) showGameView = true;
    const Engine::ViewportHandle descriptor = showGameView ? gameDescriptor : sceneDescriptor;

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
    if (descriptor && size.x > 1.0f && size.y > 1.0f) {
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
        ImGui::Image(ImTextureRef{static_cast<ImTextureID>(descriptor.value)},
                     {frameSize.x, imageHeight}, {0, 0}, {1, 1});
        viewportHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        if (!showGameView && !playing) {
            drawSceneOrientationGizmo(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                      sceneCameraYaw, sceneCameraPitch);
        }
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

Engine::Entity HierarchyPanel::draw(Engine::ScenePreset& scene, const Engine::Entity selected) {
    Engine::Entity clicked = Engine::NullEntity;
    ImGui::Begin("Hierarchy");
    drawPanelHeader("SCENE HIERARCHY");
    ImGui::SameLine(ImGui::GetWindowWidth() - 75.0f);
    if (EditorButton("+").drawSmall()) clicked = scene.createGameObject();
    ImGui::SameLine(ImGui::GetWindowWidth() - 45.0f);
    ImGui::TextDisabled("%zu", scene.editor().size());
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
        entities.reserve(scene.editor().size());
        scene.editor().view<>([&](const Engine::Entity entity) {
            entities.push_back(entity);
            if (scene.editor().has<Engine::UUIDComponent>(entity)) {
                byUuid.emplace(scene.editor().get<Engine::UUIDComponent>(entity).value, entity);
            }
        });
        std::ranges::sort(entities);

        std::unordered_map<Engine::Entity, std::vector<Engine::Entity>> children;
        std::vector<Engine::Entity> roots;
        for (const Engine::Entity entity : entities) {
            if (scene.editor().has<Engine::ParentComponent>(entity)) {
                const Engine::UUID parent = scene.editor().get<Engine::ParentComponent>(entity).parent;
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

bool ComponentsPanel::draw(Engine::ScenePreset& scene, const Engine::Entity selected) {
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
    if (scene.editor().valid(selected) && scene.editor().has<Engine::NameComponent>(selected)) {
        const auto readScene = scene.editor();
        const auto& name = readScene.get<Engine::NameComponent>(selected).value;
        char editableName[260]{};
        std::snprintf(editableName, sizeof(editableName), "%s", name.c_str());
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputTextWithHint("##object-name", "Object name", editableName, sizeof(editableName)) &&
            editableName[0] != '\0') {
            scene.editor().modify<Engine::NameComponent>(selected, [&](auto& value) {
                value.value = editableName;
            });
        }
    }
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen) &&
        scene.editor().valid(selected) && scene.editor().has<Engine::Transform>(selected)) {
        TransformFields{scene.editor(), selected}.draw();
    }
    if (scene.editor().valid(selected) &&
        scene.editor().has<Engine::SmokeEmitterComponent>(selected) &&
        ImGui::CollapsingHeader("Smoke Emitter", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Keep the UI editing a temporary copy. The component is committed
        // once, after all controls have been drawn, so observers receive one
        // coherent change notification per frame.
        const auto readScene = scene.editor();
        const auto& source =
            readScene.get<Engine::SmokeEmitterComponent>(selected).emitter;
        auto emitter = source;
        const bool hasColorPicker =
            readScene.has<Engine::ColorPickerComponent>(selected);
        if (hasColorPicker) {
            emitter.color = readScene.get<Engine::ColorPickerComponent>(selected).color;
        }

        bool changed = false;
        bool colorChanged = false;
        const auto drawParticleFloat = [](const char* label, const char* id,
                                          float* value, const float speed,
                                          const float min, const float max,
                                          const char* format) {
            ImGui::TextDisabled("%s", label);
            ImGui::SetNextItemWidth(-1.0f);
            return ImGui::DragFloat(id, value, speed, min, max, format);
        };

        changed |= drawParticleFloat("Spawn Rate", "##particle-spawn-rate",
                                     &emitter.spawnRate, 1.0f, 0.0f, 5000.0f,
                                     "%.0f particles/s");
        changed |= drawParticleFloat("Minimum Lifetime", "##particle-min-lifetime",
                                     &emitter.minLifeTime, 0.01f, 0.0f, 60.0f,
                                     "%.2f s");
        changed |= drawParticleFloat("Maximum Lifetime", "##particle-max-lifetime",
                                     &emitter.maxLifeTime, 0.01f, 0.0f, 60.0f,
                                     "%.2f s");
        changed |= drawParticleFloat("Minimum Size", "##particle-min-size",
                                     &emitter.minSize, 0.01f, 0.0f, 10.0f,
                                     "%.2f");
        changed |= drawParticleFloat("Maximum Size", "##particle-max-size",
                                     &emitter.maxSize, 0.01f, 0.0f, 10.0f,
                                     "%.2f");
        changed |= drawParticleFloat("Buoyancy", "##smoke-buoyancy",
                                     &emitter.buoyancy, 0.05f, 0.0f, 30.0f,
                                     "%.2f");
        changed |= drawParticleFloat("Air Drag", "##smoke-drag",
                                     &emitter.drag, 0.02f, 0.0f, 10.0f,
                                     "%.2f");
        changed |= drawParticleFloat("Turbulence", "##smoke-turbulence",
                                     &emitter.turbulence, 0.02f, 0.0f, 10.0f,
                                     "%.2f");
        changed |= drawParticleFloat("Collision Radius", "##smoke-collision-radius",
                                     &emitter.collisionRadius, 0.005f, 0.0f, 2.0f,
                                     "%.3f");

        float minVelocity[3] = {
            emitter.minVelocity.x(), emitter.minVelocity.y(), emitter.minVelocity.z()
        };
        float maxVelocity[3] = {
            emitter.maxVelocity.x(), emitter.maxVelocity.y(), emitter.maxVelocity.z()
        };
        if (ImGui::DragFloat3("Min Velocity", minVelocity, 0.05f, -100.0f, 100.0f)) {
            emitter.minVelocity = {minVelocity[0], minVelocity[1], minVelocity[2]};
            changed = true;
        }
        if (ImGui::DragFloat3("Max Velocity", maxVelocity, 0.05f, -100.0f, 100.0f)) {
            emitter.maxVelocity = {maxVelocity[0], maxVelocity[1], maxVelocity[2]};
            changed = true;
        }

        float color[4] = {
            emitter.color.r(), emitter.color.g(), emitter.color.b(), emitter.color.a()
        };
        ImGui::TextDisabled("Color");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::ColorEdit4("##particle-color", color, ImGuiColorEditFlags_AlphaBar)) {
            emitter.color = Engine::Color{color[0], color[1], color[2], color[3]};
            changed = true;
            colorChanged = true;
        }

        // Enforce valid ranges even when values are entered from the keyboard.
        emitter.minLifeTime = std::max(0.0f, emitter.minLifeTime);
        emitter.maxLifeTime = std::max(emitter.minLifeTime, emitter.maxLifeTime);
        emitter.minSize = std::max(0.0f, emitter.minSize);
        emitter.maxSize = std::max(emitter.minSize, emitter.maxSize);
        emitter.spawnRate = std::max(0.0f, emitter.spawnRate);
        emitter.buoyancy = std::max(0.0f, emitter.buoyancy);
        emitter.drag = std::max(0.0f, emitter.drag);
        emitter.turbulence = std::max(0.0f, emitter.turbulence);
        emitter.collisionRadius = std::max(0.0f, emitter.collisionRadius);

        if (changed) {
            scene.editor().modify<Engine::SmokeEmitterComponent>(selected,
                [&](auto& component) {
                    component.emitter = emitter;
                });
            if (colorChanged && hasColorPicker) {
                scene.editor().modify<Engine::ColorPickerComponent>(selected,
                    [&](auto& component) {
                        component.color = emitter.color;
                    });
            }
        }
    }
    if (scene.editor().valid(selected) && scene.editor().has<Engine::ScriptComponent>(selected) &&
        ImGui::CollapsingHeader("Script", ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto readScene = scene.editor();
        const auto& script = readScene.get<Engine::ScriptComponent>(selected);
        char className[260]{};
        std::snprintf(className, sizeof(className), "%s", script.className.c_str());
        ImGui::TextDisabled("C++ script class");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText("##script-class", className, sizeof(className))) {
            scene.editor().modify<Engine::ScriptComponent>(selected, [&](auto& value) {
                value.className = className;
                value.reset();
            });
        }
        bool enabled = script.enabled;
        if (ImGui::Checkbox("Enabled##script", &enabled)) {
            scene.editor().modify<Engine::ScriptComponent>(selected, [&](auto& value) {
                value.enabled = enabled;
            });
        }
    }
    if (scene.editor().valid(selected) && scene.editor().has<Engine::ColliderComponent>(selected) &&
        ImGui::CollapsingHeader("Collider", ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto collider = scene.editor().get<Engine::ColliderComponent>(selected);
        int shape = static_cast<int>(collider.shape.index());
        const char* shapeNames[] = {"Box", "Sphere", "Capsule", "Ramp"};
        bool changed = false;
        if (ImGui::BeginCombo("Shape##collider", shapeNames[shape])) {
            for (int index = 0; index < 4; ++index) {
                if (ImGui::Selectable(shapeNames[index], shape == index)) { shape = index; changed = true; }
            }
            ImGui::EndCombo();
        }
        auto value = collider;
        if (shape != static_cast<int>(value.shape.index())) {
            value.shape = shape == 0 ? Engine::ColliderShape{Engine::BoxCollider{}} :
                shape == 1 ? Engine::ColliderShape{Engine::SphereCollider{}} :
                shape == 2 ? Engine::ColliderShape{Engine::CapsuleCollider{}} :
                             Engine::ColliderShape{Engine::RampCollider{}};
        }
        float offset[3] = {value.offset.x(), value.offset.y(), value.offset.z()};
        if (ImGui::DragFloat3("Offset##collider", offset, 0.05f)) {
            value.offset = {offset[0], offset[1], offset[2]}; changed = true;
        }
        std::visit([&](auto& colliderShape) {
            using Shape = std::decay_t<decltype(colliderShape)>;
            if constexpr (std::is_same_v<Shape, Engine::BoxCollider>) {
                float extents[3] = {colliderShape.halfExtents.x(), colliderShape.halfExtents.y(), colliderShape.halfExtents.z()};
                if (ImGui::DragFloat3("Half Extents##collider", extents, 0.05f, 0.001f, 1000.0f)) {
                    colliderShape.halfExtents = {std::max(0.001f, extents[0]), std::max(0.001f, extents[1]), std::max(0.001f, extents[2])}; changed = true;
                }
            } else if constexpr (std::is_same_v<Shape, Engine::SphereCollider>) {
                changed |= ImGui::DragFloat("Radius##collider", &colliderShape.radius, 0.05f, 0.001f, 1000.0f);
                colliderShape.radius = std::max(0.001f, colliderShape.radius);
            } else if constexpr (std::is_same_v<Shape, Engine::CapsuleCollider>) {
                changed |= ImGui::DragFloat("Radius##collider", &colliderShape.radius, 0.05f, 0.001f, 1000.0f);
                changed |= ImGui::DragFloat("Height##collider", &colliderShape.height, 0.05f, 0.001f, 1000.0f);
                colliderShape.radius = std::max(0.001f, colliderShape.radius);
                colliderShape.height = std::max(0.001f, colliderShape.height);
            }
        }, value.shape);
        changed |= ImGui::Checkbox("Is Trigger##collider", &value.isTrigger);
        changed |= ImGui::DragFloat("Friction##collider", &value.friction, 0.01f, 0.0f, 10.0f);
        changed |= ImGui::SliderFloat("Restitution##collider", &value.restitution, 0.0f, 1.0f);
        value.friction = std::max(0.0f, value.friction);
        value.restitution = std::clamp(value.restitution, 0.0f, 1.0f);
        if (changed) scene.editor().modify<Engine::ColliderComponent>(selected,
            [&](auto& component) { component = value; });
    }
    if (scene.editor().valid(selected) && scene.editor().has<Engine::RigidbodyComponent>(selected) &&
        ImGui::CollapsingHeader("Rigidbody", ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto rigidbody = scene.editor().get<Engine::RigidbodyComponent>(selected);
        auto value = rigidbody;
        int type = static_cast<int>(value.type);
        const char* typeNames[] = {"Static", "Dynamic", "Kinematic"};
        if (ImGui::BeginCombo("Type##rigidbody", typeNames[type])) {
            for (int index = 0; index < 3; ++index) {
                if (ImGui::Selectable(typeNames[index], type == index)) type = index;
            }
            ImGui::EndCombo();
        }
        value.type = static_cast<Engine::RigidbodyType>(type);
        bool changed = ImGui::Checkbox("Use Gravity##rigidbody", &value.useGravity);
        changed |= ImGui::DragFloat("Mass##rigidbody", &value.mass, 0.05f, 0.001f, 100000.0f);
        value.mass = std::max(0.001f, value.mass);
        if (changed || value.type != rigidbody.type) {
            scene.editor().modify<Engine::RigidbodyComponent>(selected,
                [&](auto& component) { component = value; });
        }
    }
    if (scene.editor().valid(selected) && scene.editor().has<Engine::ColorPickerComponent>(selected) &&
        ImGui::CollapsingHeader("Color Picker", ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto readScene = scene.editor();
        const auto& picker =
            readScene.get<Engine::ColorPickerComponent>(selected);
        float rgba[4] = {picker.color.r(), picker.color.g(), picker.color.b(), picker.color.a()};
        if (ImGui::ColorEdit4("Color", rgba, ImGuiColorEditFlags_AlphaBar)) {
            scene.editor().modify<Engine::ColorPickerComponent>(selected, [&](auto& component) {
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
        const bool hasScript = scene.editor().has<Engine::ScriptComponent>(selected);
        const bool hasColorPicker = scene.editor().has<Engine::ColorPickerComponent>(selected);
        const bool hasCollider = scene.editor().has<Engine::ColliderComponent>(selected);
        const bool hasRigidbody = scene.editor().has<Engine::RigidbodyComponent>(selected);
        const bool hasSmokeEmitter = scene.editor().has<Engine::SmokeEmitterComponent>(selected);
        if (ImGui::MenuItem("Script", nullptr, false, !hasScript)) {
            scene.editor().add<Engine::ScriptComponent>(selected);
            ImGui::CloseCurrentPopup();
        }
        if (hasScript) ImGui::TextDisabled("Script component already added");
        if (ImGui::MenuItem("Color Picker", nullptr, false, !hasColorPicker)) {
            scene.editor().add<Engine::ColorPickerComponent>(selected);
            ImGui::CloseCurrentPopup();
        }
        if (hasColorPicker) ImGui::TextDisabled("Color Picker component already added");
        if (ImGui::MenuItem("Collider", nullptr, false, !hasCollider)) {
            scene.editor().add<Engine::ColliderComponent>(selected);
            ImGui::CloseCurrentPopup();
        }
        if (hasCollider) ImGui::TextDisabled("Collider component already added");
        if (ImGui::MenuItem("Rigidbody", nullptr, false, !hasRigidbody)) {
            scene.editor().add<Engine::RigidbodyComponent>(selected);
            ImGui::CloseCurrentPopup();
        }
        if (hasRigidbody) ImGui::TextDisabled("Rigidbody component already added");
        if (ImGui::MenuItem("Smoke Emitter", nullptr, false, !hasSmokeEmitter)) {
            scene.editor().add<Engine::SmokeEmitterComponent>(selected);
            ImGui::CloseCurrentPopup();
        }
        if (hasSmokeEmitter) ImGui::TextDisabled("Smoke Emitter component already added");
        ImGui::EndPopup();
    }
    if (EditorButton("Attach C++ Script", {-1.0f, 0.0f}).draw()) ImGui::OpenPopup("Attach C++ Script");
    if (ImGui::BeginPopupModal("Attach C++ Script", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static char attachedClassName[128]{};
        ImGui::TextUnformatted("Enter the registered C++ script class name.");
        ImGui::InputTextWithHint("Class name", "CubeMovement", attachedClassName, sizeof(attachedClassName));
        const bool validName = attachedClassName[0] != '\0';
        if (EditorButton("Attach", {100.0f, 0.0f}).draw() && validName) {
            if (!scene.editor().has<Engine::ScriptComponent>(selected)) {
                scene.editor().add<Engine::ScriptComponent>(selected);
            }
            scene.editor().modify<Engine::ScriptComponent>(selected, [&](auto& script) {
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
        if (EditorButton("Create").draw() && EditorSceneSession::createCppScript(name, error)) {
            if (!scene.editor().has<Engine::ScriptComponent>(selected)) scene.editor().add<Engine::ScriptComponent>(selected);
            scene.editor().modify<Engine::ScriptComponent>(selected, [&](auto& script) { script.className = name; script.reset(); });
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
                        scene.editor().size(), selected == Engine::NullEntity ? "None" : entityName(scene, selected));
    ImGui::End();
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
        const std::filesystem::path scenePath = EditorSceneSession::scenePath();
        if (ImGui::MenuItem("Save Scene", "Ctrl+S")) {
            try {
                std::filesystem::create_directories(scenePath.parent_path());
                const auto samples = renderer.antialiasingLevel() == Engine::AntialiasingLevel::MSAA2x ? 2u :
                    renderer.antialiasingLevel() == Engine::AntialiasingLevel::MSAA4x ? 4u : 0u;
                Engine::SceneSerializer::save(scene, scenePath, samples);
                sceneFileError.clear();
            } catch (const std::exception& error) { sceneFileError = error.what(); }
        }
        if (ImGui::MenuItem("Load Scene", "Ctrl+O")) {
            try {
                std::optional<std::uint32_t> samples;
                Engine::SceneSerializer::load(scene, scenePath, samples);
                if (samples) {
                    renderer.setAntialiasingLevel(*samples == 2 ? Engine::AntialiasingLevel::MSAA2x :
                        *samples == 4 ? Engine::AntialiasingLevel::MSAA4x : Engine::AntialiasingLevel::Off);
                    antialiasingChanged = true;
                }
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
        if (ImGui::MenuItem("Create Sphere")) {
            createdEntity = scene.createSphere();
        }
        if (ImGui::MenuItem("Create Ramp")) {
            createdEntity = scene.createRamp();
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

int main() {
    try {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) throw std::runtime_error(SDL_GetError());
        SDL_Window* window = SDL_CreateWindow("GamEngine Editor - Particles", 1280, 720,
                                              SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
        if (!window) throw std::runtime_error(SDL_GetError());

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
        EditorStyle::apply();

        Engine::ScenePreset scene(Engine::SceneType::Particles);
        Engine::ScriptSystem scriptSystem{Engine::ScriptRegistry::instance()};
        Engine::PhysicsSystem physicsSystem{};
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
            const Engine::EditorEventState events = renderer.pollEditorEvents();
            if (events.quitRequested) running = false;
            if (events.togglePlay) {
                if (EditorSceneSession::setPlayMode(!playing, scene, playSceneSnapshot, playModeError,
                                EditorSceneSession::msaaSampleCount(renderer))) {
                        playing = !playing;
                        paused = false;
                        showGameView = playing;
                        rendererReloadPending = !playing;
                    }
            }
            if (events.togglePause && playing) paused = !paused;
            if (!running) break;

            // Antialiasing changes recreate render-target resources. Ordinary
            // scene edits are synchronized below without rebuilding the UI or
            // swapchain.
            if (rendererReloadPending) {
                renderer.reloadScene(scene, window);
                rendererReloadPending = false;
            }

            const std::uint64_t sceneStructureBeforeUi = scene.editor().structuralRevision();
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
            if (sceneLoaded) {
                // Loading replaces the registry, so any selection from the
                // previous scene is stale before the hierarchy/inspector are
                // drawn for this frame.
                selectedEntity = Engine::NullEntity;
                renderer.setEditorSelection(selectedEntity);
                rendererReloadPending = true;
            }
            if (playToggleRequested && EditorSceneSession::setPlayMode(!playing, scene, playSceneSnapshot, playModeError,
                                                   EditorSceneSession::msaaSampleCount(renderer))) {
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
            EditorStyle::configureDockLayout();
            if (const Engine::Entity clicked = HierarchyPanel::draw(scene, selectedEntity);
                clicked != Engine::NullEntity) {
                selectedEntity = clicked;
                renderer.setEditorSelection(selectedEntity);
            }
            const bool sceneCameraInput = drawViewport(
                renderer.gameViewport(), renderer.sceneViewport(), renderer.editorCameraYaw(),
                renderer.editorCameraPitch(), showGameView, playing);
            const bool inspectorConsumesMouseWheel = ComponentsPanel::draw(scene, selectedEntity);
            drawStatusBar(scene, selectedEntity, playing, paused);
            renderer.setEditorSceneCameraInput(
                sceneCameraInput && !inspectorConsumesMouseWheel);
            ImGui::Render();

            if (antialiasingChanged) rendererReloadPending = true;
            const bool sceneStructureChanged = !sceneLoaded &&
                scene.editor().structuralRevision() != sceneStructureBeforeUi;
            if (sceneStructureChanged) renderer.synchronizeScene(scene);
            if (playing && !paused) {
                physicsSystem.update(scene, static_cast<float>(Engine::Time::deltaTime()));
                scriptSystem.update(scene, static_cast<float>(Engine::Time::deltaTime()));
            }
            renderer.renderFrame();

            if (const auto elapsed = std::chrono::steady_clock::now() - start; elapsed < targetFrame) std::this_thread::sleep_for(targetFrame - elapsed);
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
