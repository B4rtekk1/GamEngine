namespace {
    enum class TerrainToolMode { None, Sculpt, Paint, Details };

    TerrainToolMode activeTerrainTool(const TerrainSculptState& state) {
        if (state.enabled) return TerrainToolMode::Sculpt;
        if (state.paintEnabled) return TerrainToolMode::Paint;
        if (state.grassEnabled) return TerrainToolMode::Details;
        return TerrainToolMode::None;
    }

    void setTerrainTool(TerrainSculptState& state, const TerrainToolMode mode) {
        state.enabled = mode == TerrainToolMode::Sculpt;
        state.paintEnabled = mode == TerrainToolMode::Paint;
        state.grassEnabled = mode == TerrainToolMode::Details;
    }

    bool terrainModeButton(const char* label, const char* hint, const bool active) {
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, {0.06F, 0.48F, 0.59F, 1.0F});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.10F, 0.62F, 0.70F, 1.0F});
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.05F, 0.38F, 0.48F, 1.0F});
        }
        const bool clicked = ImGui::Button(label, {ImGui::GetContentRegionAvail().x, 42.0F});
        if (active) ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", hint);
        return clicked;
    }

    void panelSection(const char* title, const char* description) {
        ImGui::Spacing();
        ImGui::TextColored({0.55F, 0.80F, 0.92F, 1.0F}, "%s", title);
        if (description != nullptr) ImGui::TextDisabled("%s", description);
        ImGui::Separator();
        ImGui::Spacing();
    }

    void fullWidthSlider(const char* label, float* value, const float minimum,
                         const float maximum, const char* format) {
        ImGui::TextDisabled("%s", label);
        ImGui::SetNextItemWidth(-1.0F);
        Editor::Controls::sliderFloat((std::string{"##"} + label).c_str(), value, minimum, maximum, format);
    }

    void drawSculptSettings(TerrainSculptState& state) {
        panelSection("SCULPT BRUSH", "Shape the terrain heightmap");
        constexpr const char* modes[]{"Raise", "Lower", "Smooth", "Flatten"};
        int mode = static_cast<int>(state.mode);
        ImGui::TextDisabled("Action");
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::Combo("##terrain-sculpt-action", &mode, modes, 4))
            state.mode = static_cast<Engine::TerrainSculptMode>(mode);

        fullWidthSlider("Brush size", &state.radius, 0.25F, 8.0F, "%.1f m");
        fullWidthSlider("Strength", &state.strength, 0.1F, 12.0F, "%.1f");

        constexpr const char* falloffs[]{"Smooth", "Linear", "Sharp"};
        int falloff = static_cast<int>(state.falloff);
        ImGui::TextDisabled("Falloff");
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::Combo("##terrain-sculpt-falloff", &falloff, falloffs, 3))
            state.falloff = static_cast<Engine::TerrainBrushFalloff>(falloff);
        fullWidthSlider("Spacing", &state.spacing, 0.05F, 1.0F, "%.2f");

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.075F, 0.105F, 0.135F, 1.0F});
        ImGui::BeginChild("##sculpt-help", {0.0F, 56.0F}, true,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::TextDisabled("Shift  Invert raise / lower");
        ImGui::TextDisabled("Ctrl   Pick flatten height");
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    void drawPaintSettings(Engine::ScenePreset& scene, Engine::Assets::Content& content,
                           const Engine::Entity selected, Engine::Renderer& renderer,
                           TerrainSculptState& state) {
        panelSection("MATERIAL PAINT", "Blend texture layers on the terrain");
        constexpr const char* layers[]{"Layer 1", "Layer 2", "Layer 3", "Base layer"};
        ImGui::TextDisabled("Active layer");
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::Combo("##terrain-paint-layer", &state.paintLayer, layers, 4);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The base layer fills unpainted areas; Layers 1-3 are brush-painted.");

        const auto& terrainRenderer = scene.editor().get<Engine::MeshRenderer>(selected);
        const bool hasTexture = terrainRenderer.mesh && terrainRenderer.mesh->images.size() >
                                static_cast<std::size_t>(state.paintLayer);
        ImGui::TextDisabled("Layer texture");
        ImGui::PushStyleColor(ImGuiCol_Button, hasTexture
            ? ImVec4{0.10F, 0.25F, 0.29F, 1.0F} : ImVec4{0.11F, 0.125F, 0.158F, 1.0F});
        const std::string textureLabel = hasTexture
            ? "Texture assigned  -  drop to replace"
            : "Drop a texture asset here";
        ImGui::Button(textureLabel.c_str(), {-1.0F, 46.0F});
        ImGui::PopStyleColor();
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                    Editor::AssetDragDrop::texturePayload)) {
                const auto texture = content.texture(Editor::AssetDragDrop::texturePath(*payload));
                if (texture && texture->width > 0 && texture->height > 0 &&
                    !texture->rgbaPixels.empty() && terrainRenderer.mesh) {
                    auto mesh = std::make_shared<Engine::Mesh>(*terrainRenderer.mesh);
                    mesh->images.resize(std::max(mesh->images.size(),
                        static_cast<std::size_t>(state.paintLayer + 1)));
                    mesh->images[state.paintLayer] = {texture->width, texture->height,
                                                     texture->rgbaPixels};
                    scene.editor().modify<Engine::MeshRenderer>(selected, [&](auto& component) {
                        component.mesh = mesh;
                        for (int layer = 0; layer < 4; ++layer)
                            component.material.terrainLayerTextures[layer] =
                                layer < static_cast<int>(mesh->images.size()) ? layer : -1;
                        component.material.terrainLayered = true;
                    });
                    renderer.synchronizeScene(scene);
                }
            }
            ImGui::EndDragDropTarget();
        }
        fullWidthSlider("Brush size", &state.radius, 0.25F, 8.0F, "%.1f m");
        fullWidthSlider("Opacity", &state.paintOpacity, 0.02F, 1.0F, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Lower values blend with existing layers; 1.0 replaces them.");
    }

    void drawDetailSettings(Engine::ScenePreset& scene, Engine::Assets::Content& content,
                            const Engine::Entity selected, TerrainSculptState& state) {
        panelSection("TREES & GRASS", "Scatter or erase detail meshes");
        const float halfWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5F;
        if (!state.grassErase) {
            ImGui::PushStyleColor(ImGuiCol_Button, {0.08F, 0.42F, 0.32F, 1.0F});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.10F, 0.55F, 0.40F, 1.0F});
        }
        if (ImGui::Button("+  Add details", {halfWidth, 36.0F})) state.grassErase = false;
        if (!state.grassErase) ImGui::PopStyleColor(2);
        ImGui::SameLine();
        if (state.grassErase) {
            ImGui::PushStyleColor(ImGuiCol_Button, {0.52F, 0.20F, 0.23F, 1.0F});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.66F, 0.25F, 0.28F, 1.0F});
        }
        if (ImGui::Button("-  Erase details", {halfWidth, 36.0F})) state.grassErase = true;
        if (state.grassErase) ImGui::PopStyleColor(2);

        const bool hasDetails = scene.editor().has<Engine::TerrainGrassComponent>(selected) &&
                                scene.editor().get<Engine::TerrainGrassComponent>(selected).hasPrefab();
        std::string prefabLabel = "Drop a tree or grass model here";
        if (hasDetails) {
            const auto& details = scene.editor().get<Engine::TerrainGrassComponent>(selected);
            const std::string name = details.mesh->sourcePath.filename().string();
            prefabLabel = name.empty() ? "Detail mesh assigned" : name;
            ImGui::TextDisabled("%zu placed instances", details.instances.size());
        }
        ImGui::TextDisabled("Detail prefab");
        ImGui::PushStyleColor(ImGuiCol_Button, hasDetails
            ? ImVec4{0.10F, 0.25F, 0.20F, 1.0F} : ImVec4{0.11F, 0.125F, 0.158F, 1.0F});
        ImGui::Button(prefabLabel.c_str(), {-1.0F, 48.0F});
        ImGui::PopStyleColor();
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                    Editor::AssetDragDrop::modelPayload)) {
                const auto prefab = Engine::Prefab::model(
                    content, Editor::AssetDragDrop::modelPath(*payload));
                Engine::TerrainGrassComponent component;
                if (scene.editor().has<Engine::TerrainGrassComponent>(selected))
                    component = scene.editor().get<Engine::TerrainGrassComponent>(selected);
                component.mesh = prefab.mesh();
                component.material = prefab.material();
                component.castShadow = false;
                if (scene.editor().has<Engine::TerrainGrassComponent>(selected))
                    scene.editor().remove<Engine::TerrainGrassComponent>(selected);
                scene.editor().add<Engine::TerrainGrassComponent>(selected, std::move(component));
            }
            ImGui::EndDragDropTarget();
        }
        fullWidthSlider("Brush size", &state.radius, 0.25F, 8.0F, "%.1f m");
        fullWidthSlider("Density", &state.grassDensity, 0.1F, 12.0F, "%.1f");
        fullWidthSlider("Minimum scale", &state.grassMinimumScale, 0.05F, 4.0F, "%.2f");
        fullWidthSlider("Maximum scale", &state.grassMaximumScale,
                        state.grassMinimumScale, 4.0F, "%.2f");
        ImGui::Checkbox("Random rotation", &state.grassRandomYaw);
    }
}

void drawTerrainToolsPanel(Engine::ScenePreset& scene, Engine::Assets::Content& content,
                           const Engine::Entity selected, Engine::Renderer& renderer,
                           TerrainSculptState& state, const bool playing, bool& isOpen) {
    ImGui::SetNextWindowSizeConstraints({285.0F, 300.0F},
                                        {std::numeric_limits<float>::max(),
                                         std::numeric_limits<float>::max()});
    const bool panelVisible = ImGui::Begin("Terrain Tools", &isOpen);
    if (!isOpen) {
        if (state.strokeActive) finishTerrainStroke(scene, state);
        setTerrainTool(state, TerrainToolMode::None);
        ImGui::End();
        return;
    }
    if (!panelVisible) {
        ImGui::End();
        return;
    }

    const bool terrainSelected = !playing && selected != Engine::NullEntity &&
        scene.editor().valid(selected) && scene.editor().has<Engine::TerrainComponent>(selected) &&
        scene.editor().has<Engine::MeshRenderer>(selected);
    if (!terrainSelected) {
        if (state.strokeActive) finishTerrainStroke(scene, state);
        setTerrainTool(state, TerrainToolMode::None);
        ImGui::Dummy({0.0F, 24.0F});
        const float center = ImGui::GetContentRegionAvail().x * 0.5F;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0F, center - 28.0F));
        ImGui::TextColored({0.35F, 0.68F, 0.76F, 1.0F}, "TERRAIN");
        ImGui::Spacing();
        ImGui::TextWrapped(playing ? "Terrain editing is unavailable in Play mode."
                                  : "Select a terrain object to start sculpting, painting, or placing details.");
        ImGui::End();
        return;
    }

    const auto& terrain = scene.editor().get<Engine::TerrainComponent>(selected);
    ImGui::TextColored({0.55F, 0.82F, 0.93F, 1.0F}, "TERRAIN EDITOR");
    ImGui::TextDisabled("%u x %u heightmap  |  %.0f x %.0f m",
                        terrain.resolution, terrain.resolution, terrain.width, terrain.depth);
    ImGui::Spacing();

    const TerrainToolMode active = activeTerrainTool(state);
    if (terrainModeButton("Sculpt", "Raise, lower, smooth, and flatten terrain",
                          active == TerrainToolMode::Sculpt)) {
        const TerrainToolMode next = active == TerrainToolMode::Sculpt
            ? TerrainToolMode::None : TerrainToolMode::Sculpt;
        setTerrainTool(state, next);
        auto mesh = std::make_shared<Engine::Mesh>(terrain.createMesh(
            next == TerrainToolMode::Sculpt ? 0U : state.previewLod));
        scene.editor().modify<Engine::MeshRenderer>(selected,
            [&](auto& component) { component.mesh = std::move(mesh); });
        renderer.synchronizeScene(scene);
    }
    if (terrainModeButton("Paint Materials", "Blend up to four terrain texture layers",
                          active == TerrainToolMode::Paint))
        setTerrainTool(state, active == TerrainToolMode::Paint
            ? TerrainToolMode::None : TerrainToolMode::Paint);
    if (terrainModeButton("Trees & Grass", "Scatter foliage and other detail meshes",
                          active == TerrainToolMode::Details))
        setTerrainTool(state, active == TerrainToolMode::Details
            ? TerrainToolMode::None : TerrainToolMode::Details);

    if (state.enabled) drawSculptSettings(state);
    else if (state.paintEnabled) drawPaintSettings(scene, content, selected, renderer, state);
    else if (state.grassEnabled) drawDetailSettings(scene, content, selected, state);
    else {
        panelSection("TERRAIN PREVIEW", "Display quality while editing");
        int lod = static_cast<int>(state.previewLod);
        ImGui::TextDisabled("Level of detail");
        ImGui::SetNextItemWidth(-1.0F);
        if (Editor::Controls::sliderInt("##terrain-preview-lod", &lod, 0, 5, "%d")) {
            state.previewLod = static_cast<std::uint32_t>(lod);
            auto mesh = std::make_shared<Engine::Mesh>(terrain.createMesh(state.previewLod));
            scene.editor().modify<Engine::MeshRenderer>(selected,
                [&](auto& component) { component.mesh = std::move(mesh); });
            renderer.synchronizeScene(scene);
        }
        ImGui::TextWrapped("Choose a tool above, then paint directly in the Scene View.");
    }
    ImGui::End();
}
