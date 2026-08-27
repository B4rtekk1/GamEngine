Engine::Entity HierarchyPanel::draw(Engine::ScenePreset &scene, const Engine::Entity selected,
                                    Action &action, Engine::Entity &actionEntity, const bool canPaste) {
    Engine::Entity clicked = Engine::NullEntity;
    action = Action::None;
    actionEntity = Engine::NullEntity;
    ImGui::Begin("Hierarchy");
    if (EditorButton("+  New Object", {-1.0F, 0.0F}).draw()) {
        clicked = scene.createGameObject();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Create an empty game object");
    }
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

    if (ImGui::TreeNodeEx("Scene", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth)) {
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
        std::ranges::sort(entities);

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
                drawContextMenu();
                return;
            }
            const ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth |
                                             (selected == entity ? ImGuiTreeNodeFlags_Selected : 0);
            const bool open = ImGui::TreeNodeEx(label, flags);
            if (ImGui::IsItemClicked()) {
                clicked = entity;
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
        ImGui::TreePop();
    }

    // Allow pasting from empty space in the hierarchy, without requiring an
    // object to be selected or right-clicked first.
    if (ImGui::BeginPopupContextWindow("##hierarchy-context", ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::MenuItem("Paste", nullptr, false, canPaste)) {
            action = Action::Paste;
        }
        ImGui::EndPopup();
    }

    ImGui::End();
    return clicked;
}
