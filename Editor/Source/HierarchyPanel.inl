Engine::Entity HierarchyPanel::draw(Engine::ScenePreset &scene, Engine::Assets::Content& content,
                                    const Engine::Entity selected, Action &action,
                                    Engine::Entity &actionEntity, const bool canPaste,
                                    const bool disabled, bool& isOpen) {
    Engine::Entity clicked = Engine::NullEntity;
    static std::string assetDropError;
    action = Action::None;
    actionEntity = Engine::NullEntity;
    const auto createObjectMenu = [&] {
        if (ImGui::MenuItem("Empty Game Object", "Ctrl+Shift+N")) {
            clicked = scene.createGameObject();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("3D Object/Cube")) clicked = scene.createCube();
        if (ImGui::MenuItem("3D Object/Sphere")) clicked = scene.createSphere();
        if (ImGui::MenuItem("3D Object/Plane")) clicked = scene.createPlane();
        if (ImGui::MenuItem("3D Object/Ramp")) clicked = scene.createRamp();
        ImGui::Separator();
        if (ImGui::MenuItem("Light/Directional Light")) clicked = scene.createLight();
        if (ImGui::MenuItem("Terrain")) clicked = scene.createTerrain();
    };
    const auto acceptModelDrop = [&](const Engine::Entity parent = Engine::NullEntity) {
        if (disabled) return;
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload(Editor::AssetDragDrop::modelPayload)) {
            try {
                clicked = Editor::AssetDragDrop::instantiateModel(
                    scene, content, Editor::AssetDragDrop::modelPath(*payload));
                if (parent != Engine::NullEntity && clicked != Engine::NullEntity &&
                    scene.editor().valid(parent) && scene.editor().valid(clicked) &&
                    scene.editor().has<Engine::UUIDComponent>(parent)) {
                    const Engine::ParentComponent link{
                        .parent = scene.editor().get<Engine::UUIDComponent>(parent).value
                    };
                    if (scene.editor().has<Engine::ParentComponent>(clicked)) {
                        scene.editor().modify<Engine::ParentComponent>(
                            clicked, [&](auto& component) { component = link; });
                    } else {
                        scene.editor().add<Engine::ParentComponent>(clicked, link);
                    }
                }
                assetDropError.clear();
            } catch (const std::exception& exception) {
                assetDropError = exception.what();
            }
        }
    };
    ImGui::Begin("Hierarchy", &isOpen);
    ImGui::BeginDisabled(disabled);
    if (EditorButton("+  New Object...", {-1.0F, 0.0F}).draw()) {
        ImGui::OpenPopup("Create Object");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Create an object in the scene");
    }
    if (ImGui::BeginPopup("Create Object")) {
        createObjectMenu();
        ImGui::EndPopup();
    }
    ImGui::EndDisabled();
    static char filter[64] = {};
    ImGui::SetNextItemWidth(-1.0F);
    const ImVec2 framePadding = ImGui::GetStyle().FramePadding;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {framePadding.x + 18.0F, framePadding.y});
    ImGui::InputTextWithHint("##hierarchy-filter", "Search objects...", filter, sizeof(filter));
    const ImVec2 searchMin = ImGui::GetItemRectMin();
    const ImVec2 searchMax = ImGui::GetItemRectMax();
    ImGui::PopStyleVar();
    drawSearchIcon(searchMin, searchMax);
    ImGui::Spacing();
    ImGui::TextDisabled("Right-click an object for more actions");
    ImGui::Spacing();

    const bool sceneOpen = ImGui::TreeNodeEx(
        "Scene", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
    if (ImGui::BeginDragDropTarget()) {
        acceptModelDrop();
        ImGui::EndDragDropTarget();
    }
    if (sceneOpen) {
        const auto entityLabel = [&](const char *name, const Engine::Entity entity) {
            if (entity != Engine::NullEntity) {
                if (!containsCaseInsensitive(name, filter)) {
                    return;
                }
                char label[64];
                std::snprintf(label, sizeof(label), "%s  (%u)", name,
                              Engine::entityIndex(entity));
                if (ImGui::Selectable(label, selected == entity)) {
                    clicked = entity;
                }
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
        std::unordered_map<Engine::Entity, std::vector<Engine::Entity> > children;
        std::vector<Engine::Entity> roots;
        for (const Engine::Entity entity: entities) {
            if (scene.editor().has<Engine::ParentComponent>(entity)) {
                const Engine::UUID parent = scene.editor().get<Engine::ParentComponent>(entity).parent;
                if (const auto found = byUuid.find(parent); found != byUuid.end()) {
                    children[found->second].push_back(entity);
                    continue;
                }
            }
            roots.push_back(entity);
        }

        const auto sortByHierarchyOrder = [&](std::vector<Engine::Entity>& siblings) {
            std::ranges::sort(siblings, [&](const Engine::Entity left, const Engine::Entity right) {
                const auto orderOf = [&](const Engine::Entity entity) {
                    return scene.editor().has<Engine::HierarchyOrderComponent>(entity)
                               ? scene.editor().get<Engine::HierarchyOrderComponent>(entity).value
                               : std::numeric_limits<std::uint32_t>::max();
                };
                const std::uint32_t leftOrder = orderOf(left);
                const std::uint32_t rightOrder = orderOf(right);
                return leftOrder == rightOrder ? left < right : leftOrder < rightOrder;
            });
        };
        sortByHierarchyOrder(roots);
        for (auto& [parent, siblings] : children) {
            sortByHierarchyOrder(siblings);
        }

        const auto reorderBefore = [&](std::vector<Engine::Entity>& siblings,
                                       const Engine::Entity moved,
                                       const Engine::Entity before) {
            if (moved == before) return;
            const auto movedIt = std::ranges::find(siblings, moved);
            const auto beforeIt = std::ranges::find(siblings, before);
            if (movedIt == siblings.end() || beforeIt == siblings.end()) return;
            siblings.erase(movedIt);
            const auto insertAt = std::ranges::find(siblings, before);
            siblings.insert(insertAt, moved);
            for (std::uint32_t index = 0; index < siblings.size(); ++index) {
                const Engine::Entity sibling = siblings[index];
                if (scene.editor().has<Engine::HierarchyOrderComponent>(sibling)) {
                    scene.editor().modify<Engine::HierarchyOrderComponent>(
                        sibling, [&](auto& order) { order.value = index; });
                } else {
                    scene.editor().add<Engine::HierarchyOrderComponent>(
                        sibling, Engine::HierarchyOrderComponent{.value = index});
                }
            }
        };

        Engine::Entity reorderSource = Engine::NullEntity;
        Engine::Entity reorderTarget = Engine::NullEntity;
        const auto requestReorder = [&](const Engine::Entity moved, const Engine::Entity before) {
            reorderSource = moved;
            reorderTarget = before;
        };

        std::unordered_set<Engine::Entity> visited;
        const auto drawNode = [&](auto &&self, const Engine::Entity entity) -> void {
            if (!visited.insert(entity).second) {
                return;
            }
            const char *name = entityName(scene, entity);
            if (!containsCaseInsensitive(name, filter)) {
                return;
            }
            const auto childIt = children.find(entity);
            const bool hasChildren = childIt != children.end() && !childIt->second.empty();
            char label[128];
            // Use the complete ECS handle for ImGui's hidden ID. The index
            // alone is not a stable identity because Registry recycles slots
            // and distinguishes them by generation.
            std::snprintf(label, sizeof(label), "%s##%llu", name,
                          static_cast<unsigned long long>(entity));
            const auto drawContextMenu = [&] {
                if (!ImGui::BeginPopupContextItem()) {
                    return;
                }
                if (ImGui::MenuItem("Copy")) {
                    action = Action::Copy;
                    actionEntity = entity;
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
                if (ImGui::Selectable(label, selected == entity)) {
                    clicked = entity;
                }
                if (ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload("HIERARCHY_REORDER", &entity, sizeof(entity));
                    ImGui::TextUnformatted(name);
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_REORDER")) {
                        const auto moved = *static_cast<const Engine::Entity*>(payload->Data);
                        requestReorder(moved, entity);
                    }
                    acceptModelDrop(entity);
                    ImGui::EndDragDropTarget();
                }
                drawContextMenu();
                return;
            }
            const ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth |
                                             (selected == entity ? ImGuiTreeNodeFlags_Selected : 0);
            const bool open = ImGui::TreeNodeEx(label, flags);
            if (ImGui::IsItemClicked()) {
                clicked = entity;
            }
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("HIERARCHY_REORDER", &entity, sizeof(entity));
                ImGui::TextUnformatted(name);
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_REORDER")) {
                    const auto moved = *static_cast<const Engine::Entity*>(payload->Data);
                    requestReorder(moved, entity);
                }
                acceptModelDrop(entity);
                ImGui::EndDragDropTarget();
            }
            drawContextMenu();
            if (open) {
                for (const Engine::Entity child: childIt->second) {
                    self(self, child);
                }
                ImGui::TreePop();
            }
        };
        for (const Engine::Entity entity: roots) {
            drawNode(drawNode, entity);
        }
        if (reorderSource != Engine::NullEntity && reorderTarget != Engine::NullEntity) {
            reorderBefore(roots, reorderSource, reorderTarget);
            for (auto& [parent, siblings] : children) {
                reorderBefore(siblings, reorderSource, reorderTarget);
            }
        }
        ImGui::TreePop();
    }

    // The remaining hierarchy area is a root-level drop target, matching the
    // behaviour of the Scene row without obscuring entity items.
    const ImVec2 dropArea = ImGui::GetContentRegionAvail();
    const ImVec2 dropMin = ImGui::GetCursorScreenPos();
    const ImRect dropRect{dropMin, {dropMin.x + dropArea.x,
                                   dropMin.y + std::max(dropArea.y, 28.0F)}};
    if (ImGui::BeginDragDropTargetCustom(dropRect, ImGui::GetID("##hierarchy-asset-drop"))) {
        acceptModelDrop();
        ImGui::EndDragDropTarget();
    }
    if (!assetDropError.empty()) {
        ImGui::TextColored({0.95F, 0.40F, 0.35F, 1.0F}, "%s", assetDropError.c_str());
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
