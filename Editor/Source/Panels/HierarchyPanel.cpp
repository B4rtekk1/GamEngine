#include "Editor/Panels/HierarchyPanel.h"

#include "Editor/App/EditorEntityHelpers.h"
#include "Editor/EditorUi.h"
#include "Editor/Panels/AssetDragDrop.h"
#include "Editor/Panels/ConsolePanel.h"
#include "Editor/UI/EditorIcons.h"
#include "Editor/UI/EditorTheme.h"
#include "Editor/UI/EditorWidgets.h"
#include "Engine/Assets/Content.h"
#include "Engine/ECS/Components/RigidbodyComponent.h"
#include "Engine/Scene/Components/IdentityComponents.h"
#include "Engine/Scene/SceneEditor.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
Engine::Entity pendingRename = Engine::NullEntity;

struct HierarchyTreeKey {
    const void* scene = nullptr;
    std::uint64_t structuralRevision = 0;
    std::uint64_t uuidRevision = 0;
    std::uint64_t parentRevision = 0;
    std::uint64_t orderRevision = 0;

    bool operator==(const HierarchyTreeKey&) const = default;
};

struct HierarchyTreeCache {
    HierarchyTreeKey key;
    bool valid = false;
    std::unordered_map<Engine::Entity, std::vector<Engine::Entity>> children;
    std::vector<Engine::Entity> roots;
    std::unordered_map<Engine::UUID, Engine::Entity> byUuid;
};

HierarchyTreeCache hierarchyTreeCache;
} // namespace

void HierarchyPanel::requestRename(const Engine::Entity entity) {
    pendingRename = entity;
}

Engine::Entity HierarchyPanel::draw(Engine::ScenePreset& scene, Engine::Assets::Content& content,
                                    const std::vector<Engine::Entity>& selection, Action& action,
                                    Engine::Entity& actionEntity, const bool canPaste, const bool disabled,
                                    bool& isOpen) {
    Engine::Entity clicked = Engine::NullEntity;
    const auto selected = [&](const Engine::Entity entity) {
        return std::ranges::find(selection, entity) != selection.end();
    };
    static std::string assetDropError;
    static Engine::Entity renameEntity = Engine::NullEntity;
    static std::string renameValue;
    static std::string renameError;
    static bool focusRenameInput = false;
    static std::unordered_set<Engine::Entity> hiddenEntities;
    static std::unordered_set<Engine::Entity> lockedEntities;
    bool openRenamePopup = false;
    Engine::Entity droppedHierarchyEntity = Engine::NullEntity;
    Engine::Entity hierarchyDropParent = Engine::NullEntity;
    action = Action::None;
    actionEntity = Engine::NullEntity;
    const auto canParent = [&](const Engine::Entity child) {
        if (!scene.editor().has<Engine::RigidbodyComponent>(child) ||
            scene.editor().read<Engine::RigidbodyComponent>(child).type != Engine::RigidbodyType::Dynamic) {
            return true;
        }
        const std::string message = "Cannot parent a dynamic rigid body: it is controlled by PhysX.";
        Editor::ConsolePanel::error(message);
        assetDropError = message;
        return false;
    };
    const auto createObjectMenu = [&] {
        if (ImGui::MenuItem("Empty Game Object", "Ctrl+Shift+N")) {
            clicked = scene.createGameObject();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("3D Object/Cube"))
            clicked = scene.createCube();
        if (ImGui::MenuItem("3D Object/Sphere"))
            clicked = scene.createSphere();
        if (ImGui::MenuItem("3D Object/Capsule"))
            clicked = scene.createCapsule();
        if (ImGui::MenuItem("3D Object/Plane"))
            clicked = scene.createPlane();
        if (ImGui::MenuItem("3D Object/Ramp"))
            clicked = scene.createRamp();
        if (ImGui::MenuItem("3D Object/Glass Pane"))
            clicked = scene.createGlassPane();
        ImGui::Separator();
        if (ImGui::MenuItem("Camera"))
            clicked = scene.createCamera();
        if (ImGui::MenuItem("Light/Directional Light"))
            clicked = scene.createLight();
        if (ImGui::MenuItem("Terrain"))
            clicked = scene.createTerrain();
        if (ImGui::BeginMenu("Water")) {
            if (ImGui::MenuItem("Ocean"))
                clicked = scene.createOcean();
            if (ImGui::MenuItem("Lake"))
                clicked = scene.createLake();
            if (ImGui::MenuItem("River"))
                clicked = scene.createRiver();
            ImGui::EndMenu();
        }
    };
    const auto acceptModelDrop = [&](const Engine::Entity parent = Engine::NullEntity) {
        if (disabled)
            return;
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(Editor::AssetDragDrop::modelPayload)) {
            try {
                // Loaded in the background; the parent link is applied when
                // the entity is created.
                std::optional<Engine::UUID> parentUuid;
                if (parent != Engine::NullEntity && scene.editor().valid(parent) &&
                    scene.editor().has<Engine::UUIDComponent>(parent))
                    parentUuid = scene.editor().read<Engine::UUIDComponent>(parent).value;
                Editor::AssetDragDrop::requestModel(content, Editor::AssetDragDrop::modelPath(*payload), std::nullopt,
                                                    parentUuid);
                assetDropError.clear();
            } catch (const std::exception& exception) {
                assetDropError = exception.what();
            }
        }
    };
    ImGui::Begin("Hierarchy", &isOpen);
    static char filter[64] = {};
    const float addButtonWidth = ImGui::GetFrameHeight();
    EditorUI::searchBox("##hierarchy-filter", "Search objects...", filter, sizeof(filter),
                        ImGui::GetContentRegionAvail().x - addButtonWidth - ImGui::GetStyle().ItemSpacing.x);
    ImGui::SameLine();
    ImGui::BeginDisabled(disabled);
    if (EditorUI::iconButton("##hierarchy-create", EditorUI::Icon::Plus, "Create an object in the scene",
                             {.size = {addButtonWidth, ImGui::GetFrameHeight()}})) {
        ImGui::OpenPopup("Create Object");
    }
    if (ImGui::BeginPopup("Create Object")) {
        createObjectMenu();
        ImGui::EndPopup();
    }
    ImGui::EndDisabled();
    if (!scene.hasUsablePrimaryCamera()) {
        EditorUI::iconLabel(EditorUI::Icon::Warning, EditorUI::colors().warning);
        ImGui::SameLine(0.0F, 6.0F);
        ImGui::TextColored(EditorUI::colors().warning, "No usable primary camera; using fallback");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Add a perspective CameraComponent with Transform and mark it Primary.");
        }
    }
    ImGui::Spacing();

    char sceneLabel[64]{};
    std::snprintf(sceneLabel, sizeof(sceneLabel), "Scene  (%zu)###hierarchy-scene-root", scene.editor().size());
    const bool sceneOpen =
        ImGui::TreeNodeEx(sceneLabel, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
    if (ImGui::BeginDragDropTarget()) {
        acceptModelDrop();
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
            droppedHierarchyEntity = *static_cast<const Engine::Entity*>(payload->Data);
        }
        ImGui::EndDragDropTarget();
    }
    if (sceneOpen) {
        const auto entityLabel = [&](const char* name, const Engine::Entity entity) {
            if (entity != Engine::NullEntity) {
                if (!containsCaseInsensitive(name, filter)) {
                    return;
                }
                char label[64];
                std::snprintf(label, sizeof(label), "%s  (%u)", name, Engine::entityIndex(entity));
                if (ImGui::Selectable(label, selected(entity))) {
                    clicked = entity;
                }
            }
        };

        // Parent links use persistent UUIDs, rather than recyclable ECS ids.
        // This makes the hierarchy survive save/load and entity reallocation.
        // The tree only depends on entity membership, UUIDs, parent links and
        // sibling order, so it is rebuilt when one of those revisions moves
        // instead of on every editor frame.
        HierarchyTreeCache& tree = hierarchyTreeCache;
        const auto editor = scene.editor();
        const HierarchyTreeKey key{&scene, editor.structuralRevision(),
                                   editor.componentRevision<Engine::UUIDComponent>(),
                                   editor.componentRevision<Engine::ParentComponent>(),
                                   editor.componentRevision<Engine::HierarchyOrderComponent>()};
        if (!tree.valid || tree.key != key) {
            tree.children.clear();
            tree.roots.clear();
            tree.byUuid.clear();
            auto& byUuid = tree.byUuid;
            std::vector<Engine::Entity> entities;
            entities.reserve(scene.editor().size());
            byUuid.reserve(scene.editor().size());
            scene.editor().view<>([&](const Engine::Entity entity) {
                entities.push_back(entity);
                if (scene.editor().has<Engine::UUIDComponent>(entity)) {
                    byUuid.emplace(scene.editor().read<Engine::UUIDComponent>(entity).value, entity);
                }
            });
            for (const Engine::Entity entity : entities) {
                if (scene.editor().has<Engine::ParentComponent>(entity)) {
                    const Engine::UUID parent = scene.editor().read<Engine::ParentComponent>(entity).parentUuid;
                    if (const auto found = byUuid.find(parent); found != byUuid.end()) {
                        tree.children[found->second].push_back(entity);
                        continue;
                    }
                }
                tree.roots.push_back(entity);
            }

            const auto sortByHierarchyOrder = [&](std::vector<Engine::Entity>& siblings) {
                const auto orderOf = [&](const Engine::Entity entity) {
                    return scene.editor().has<Engine::HierarchyOrderComponent>(entity)
                               ? scene.editor().read<Engine::HierarchyOrderComponent>(entity).value
                               : std::numeric_limits<std::uint32_t>::max();
                };
                std::ranges::sort(siblings, [&](const Engine::Entity left, const Engine::Entity right) {
                    const std::uint32_t leftOrder = orderOf(left);
                    const std::uint32_t rightOrder = orderOf(right);
                    return leftOrder == rightOrder ? left < right : leftOrder < rightOrder;
                });
            };
            sortByHierarchyOrder(tree.roots);
            for (auto& [parent, siblings] : tree.children) {
                sortByHierarchyOrder(siblings);
            }
            tree.key = key;
            tree.valid = true;
        }
        const auto& children = tree.children;
        const auto& byUuid = tree.byUuid;
        const auto& roots = tree.roots;

        std::unordered_set<Engine::Entity> visited;
        const auto drawRowControls = [&](const Engine::Entity entity) {
            const bool hidden = hiddenEntities.contains(entity);
            const bool locked = lockedEntities.contains(entity);
            const EditorUI::Palette& palette = EditorUI::colors();
            const ImVec2 toggleSize{ImGui::GetFontSize() + 2.0F, ImGui::GetFontSize()};
            ImGui::PushID(static_cast<int>(entity));
            if (EditorUI::iconButton(
                    "##visibility", hidden ? EditorUI::Icon::EyeOff : EditorUI::Icon::Eye,
                    hidden ? "Show in Scene View" : "Hide in Scene View",
                    {.size = toggleSize,
                     .tint = hidden ? EditorUI::withAlpha(palette.textSecondary, 0.55F) : palette.textSecondary})) {
                if (hidden)
                    hiddenEntities.erase(entity);
                else
                    hiddenEntities.insert(entity);
            }
            ImGui::SameLine(0.0F, 0.0F);
            if (EditorUI::iconButton(
                    "##lock", locked ? EditorUI::Icon::Lock : EditorUI::Icon::Unlock,
                    locked ? "Unlock editing" : "Lock editing",
                    {.size = toggleSize,
                     .tint = locked ? palette.warning : EditorUI::withAlpha(palette.textSecondary, 0.35F)})) {
                if (locked)
                    lockedEntities.erase(entity);
                else
                    lockedEntities.insert(entity);
            }
            ImGui::SameLine(0.0F, 4.0F);
            ImGui::PopID();
        };
        // Tree node text starts after the disclosure arrow. Reserve room for
        // the type icon with leading spaces and draw the icon into that gap.
        const float iconSize = ImGui::GetFontSize() * 0.92F;
        const float spaceWidth = std::max(1.0F, ImGui::CalcTextSize(" ").x);
        const int iconSpaces = static_cast<int>(std::ceil((iconSize + 5.0F) / spaceWidth));
        const auto drawTypeIcon = [&](const Engine::Entity entity) {
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            const float textX = min.x + ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.x * 2.0F;
            const EditorUI::Icon icon = EditorUI::entityIcon(scene, entity);
            ImVec4 color = EditorUI::entityIconColor(icon);
            if (hiddenEntities.contains(entity))
                color.w *= 0.45F;
            EditorUI::drawIcon(ImGui::GetWindowDrawList(), icon,
                               {std::round(textX + iconSize * 0.5F), std::round((min.y + max.y) * 0.5F)}, iconSize,
                               ImGui::GetColorU32(color));
        };
        const auto drawNode = [&](auto&& self, const Engine::Entity entity) -> void {
            if (!visited.insert(entity).second) {
                return;
            }
            const char* name = entityName(scene, entity);
            if (!containsCaseInsensitive(name, filter)) {
                return;
            }
            const auto childIt = children.find(entity);
            const bool hasChildren = childIt != children.end() && !childIt->second.empty();
            char label[128];
            // Use the complete ECS handle for ImGui's hidden ID. The index
            // alone is not a stable identity because Registry recycles slots
            // and distinguishes them by generation.
            std::snprintf(label, sizeof(label), "%*s%s##%llu", iconSpaces, "", name,
                          static_cast<unsigned long long>(entity));
            if (pendingRename == entity && !disabled) {
                renameEntity = entity;
                renameValue = name;
                renameError.clear();
                focusRenameInput = true;
                openRenamePopup = true;
                pendingRename = Engine::NullEntity;
            }
            const auto drawContextMenu = [&] {
                if (!ImGui::BeginPopupContextItem()) {
                    return;
                }
                if (ImGui::MenuItem("Copy")) {
                    action = Action::Copy;
                    actionEntity = entity;
                }
                if (ImGui::MenuItem("Rename", nullptr, false, !disabled)) {
                    renameEntity = entity;
                    renameValue = name;
                    renameError.clear();
                    focusRenameInput = true;
                    // The context menu is scoped to this hierarchy row's ID.
                    // Open the modal after leaving that scope so both calls
                    // resolve to the same ImGui popup ID.
                    openRenamePopup = true;
                }
                if (ImGui::MenuItem("Paste", nullptr, false, canPaste)) {
                    action = Action::Paste;
                    actionEntity = entity;
                }
                if (ImGui::MenuItem("Duplicate")) {
                    action = Action::Duplicate;
                    actionEntity = entity;
                }
                if (ImGui::MenuItem("Delete")) {
                    action = Action::Delete;
                    actionEntity = entity;
                }
                ImGui::EndPopup();
            };
            if (!hasChildren) {
                // Leaves are leaf tree nodes, so their icons and names line
                // up with expandable siblings instead of shifting left.
                drawRowControls(entity);
                ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                             ImGuiTreeNodeFlags_SpanAvailWidth |
                                             (selected(entity) ? ImGuiTreeNodeFlags_Selected : 0));
                if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                    clicked = entity;
                }
                drawTypeIcon(entity);
                if (ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload("HIERARCHY_ENTITY", &entity, sizeof(entity));
                    ImGui::TextUnformatted(name);
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
                        droppedHierarchyEntity = *static_cast<const Engine::Entity*>(payload->Data);
                        hierarchyDropParent = entity;
                    }
                    acceptModelDrop(entity);
                    ImGui::EndDragDropTarget();
                }
                drawContextMenu();
                return;
            }
            const ImGuiTreeNodeFlags flags =
                ImGuiTreeNodeFlags_SpanAvailWidth | (selected(entity) ? ImGuiTreeNodeFlags_Selected : 0);
            drawRowControls(entity);
            const bool open = ImGui::TreeNodeEx(label, flags | ImGuiTreeNodeFlags_OpenOnArrow);
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                clicked = entity;
            }
            drawTypeIcon(entity);
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("HIERARCHY_ENTITY", &entity, sizeof(entity));
                ImGui::TextUnformatted(name);
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
                    droppedHierarchyEntity = *static_cast<const Engine::Entity*>(payload->Data);
                    hierarchyDropParent = entity;
                }
                acceptModelDrop(entity);
                ImGui::EndDragDropTarget();
            }
            drawContextMenu();
            if (open) {
                for (const Engine::Entity child : childIt->second) {
                    self(self, child);
                }
                ImGui::TreePop();
            }
        };
        for (const Engine::Entity entity : roots) {
            drawNode(drawNode, entity);
        }
        if (!disabled && droppedHierarchyEntity != Engine::NullEntity && scene.editor().valid(droppedHierarchyEntity) &&
            hierarchyDropParent == Engine::NullEntity) {
            // Dropping on the Scene row makes the object a root again.
            if (scene.editor().has<Engine::ParentComponent>(droppedHierarchyEntity)) {
                scene.editor().remove<Engine::ParentComponent>(droppedHierarchyEntity);
            }
        } else if (!disabled && droppedHierarchyEntity != Engine::NullEntity &&
                   hierarchyDropParent != Engine::NullEntity && droppedHierarchyEntity != hierarchyDropParent &&
                   scene.editor().valid(droppedHierarchyEntity) && scene.editor().valid(hierarchyDropParent) &&
                   scene.editor().has<Engine::UUIDComponent>(hierarchyDropParent)) {
            // A proposed parent cannot be one of the dragged entity's descendants.
            bool createsCycle = false;
            for (Engine::Entity ancestor = hierarchyDropParent; ancestor != Engine::NullEntity && !createsCycle;) {
                if (ancestor == droppedHierarchyEntity) {
                    createsCycle = true;
                    break;
                }
                if (!scene.editor().has<Engine::ParentComponent>(ancestor))
                    break;
                const Engine::UUID parentUuid = scene.editor().read<Engine::ParentComponent>(ancestor).parentUuid;
                const auto found = byUuid.find(parentUuid);
                ancestor = found == byUuid.end() ? Engine::NullEntity : found->second;
            }
            if (!createsCycle && canParent(droppedHierarchyEntity)) {
                const Engine::ParentComponent link{
                    .parentUuid = scene.editor().read<Engine::UUIDComponent>(hierarchyDropParent).value};
                if (scene.editor().has<Engine::ParentComponent>(droppedHierarchyEntity)) {
                    scene.editor().patch<Engine::ParentComponent>(droppedHierarchyEntity,
                                                                  [&](auto& component) { component = link; });
                } else {
                    scene.editor().add<Engine::ParentComponent>(droppedHierarchyEntity, link);
                }
            }
        }
        ImGui::TreePop();
    }

    if (openRenamePopup) {
        ImGui::OpenPopup("Rename GameObject");
    }
    if (ImGui::BeginPopupModal("Rename GameObject", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (renameEntity == Engine::NullEntity || !scene.editor().valid(renameEntity)) {
            renameEntity = Engine::NullEntity;
            ImGui::CloseCurrentPopup();
        } else {
            char nameBuffer[260]{};
            std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", renameValue.c_str());
            if (focusRenameInput) {
                ImGui::SetKeyboardFocusHere();
                focusRenameInput = false;
            }
            const bool submitted =
                ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
            renameValue = nameBuffer;
            if (!renameError.empty()) {
                ImGui::TextColored(EditorUI::colors().error, "%s", renameError.c_str());
            }
            if ((submitted || ImGui::Button("Rename")) && !renameValue.empty()) {
                try {
                    scene.editor().rename(renameEntity, renameValue);
                    clicked = renameEntity;
                    renameEntity = Engine::NullEntity;
                    ImGui::CloseCurrentPopup();
                } catch (const std::exception& exception) {
                    renameError = exception.what();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                renameEntity = Engine::NullEntity;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    // The remaining hierarchy area is a root-level drop target, matching the
    // behaviour of the Scene row without obscuring entity items.
    const ImVec2 dropArea = ImGui::GetContentRegionAvail();
    const ImVec2 dropMin = ImGui::GetCursorScreenPos();
    const ImRect dropRect{dropMin, {dropMin.x + dropArea.x, dropMin.y + std::max(dropArea.y, 28.0F)}};
    if (ImGui::BeginDragDropTargetCustom(dropRect, ImGui::GetID("##hierarchy-asset-drop"))) {
        acceptModelDrop();
        ImGui::EndDragDropTarget();
    }
    if (!assetDropError.empty()) {
        ImGui::TextColored(EditorUI::colors().error, "%s", assetDropError.c_str());
    }

    // Allow pasting from empty space in the hierarchy, without requiring an
    // object to be selected or right-clicked first.
    if (ImGui::BeginPopupContextWindow("##hierarchy-context", ImGuiPopupFlags_NoOpenOverItems)) {
        ImGui::BeginDisabled(disabled);
        if (ImGui::BeginMenu("Create Object")) {
            createObjectMenu();
            ImGui::EndMenu();
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        if (ImGui::MenuItem("Paste", nullptr, false, canPaste)) {
            action = Action::Paste;
        }
        ImGui::EndPopup();
    }

    ImGui::End();
    return clicked;
}
