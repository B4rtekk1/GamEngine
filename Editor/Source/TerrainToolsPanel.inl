namespace {
enum class TerrainToolMode { None, Sculpt, Paint, Details };

TerrainToolMode activeTerrainTool(const TerrainSculptState& state) {
    if (state.enabled)
        return TerrainToolMode::Sculpt;
    if (state.paintEnabled)
        return TerrainToolMode::Paint;
    if (state.grassEnabled)
        return TerrainToolMode::Details;
    return TerrainToolMode::None;
}

void setTerrainTool(TerrainSculptState& state, const TerrainToolMode mode) {
    state.enabled = mode == TerrainToolMode::Sculpt;
    state.paintEnabled = mode == TerrainToolMode::Paint;
    state.grassEnabled = mode == TerrainToolMode::Details;
}

/** Square tool tile with an icon above its label, used for the main terrain modes. */
bool terrainModeTile(const char* id, const EditorUI::Icon icon, const char* label, const char* hint, const bool active,
                     const float width) {
    const EditorUI::Palette& palette = EditorUI::colors();
    const float height = ImGui::GetTextLineHeight() + 44.0F;
    const bool clicked = ImGui::InvisibleButton(id, {width, height});
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float rounding = ImGui::GetStyle().FrameRounding + 1.0F;
    draw->AddRectFilled(min, max,
                        ImGui::GetColorU32(active    ? palette.accentMuted
                                           : hovered ? palette.controlHover
                                                     : palette.surface),
                        rounding);
    draw->AddRect(min, max, ImGui::GetColorU32(active ? palette.accent : palette.border), rounding);
    const ImVec4 foreground = active ? palette.accentHover : hovered ? palette.textPrimary : palette.textSecondary;
    EditorUI::drawIcon(draw, icon, {(min.x + max.x) * 0.5F, min.y + 20.0F}, 22.0F, ImGui::GetColorU32(foreground));
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    draw->AddText({(min.x + max.x - textSize.x) * 0.5F, max.y - textSize.y - 7.0F},
                  ImGui::GetColorU32(active ? palette.textPrimary : palette.textSecondary), label);
    if (hovered)
        ImGui::SetTooltip("%s", hint);
    return clicked;
}

/**
 * Asset drop target drawn as a dashed box. It lights up while a drag is in
 * progress; the caller attaches BeginDragDropTarget to this item.
 */
void assetDropZone(const char* id, const EditorUI::Icon icon, const char* label, const bool assigned) {
    const EditorUI::Palette& palette = EditorUI::colors();
    const float height = 48.0F;
    ImGui::InvisibleButton(id, {ImGui::GetContentRegionAvail().x, height});
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const bool dragging = ImGui::GetDragDropPayload() != nullptr;
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec4 edge = dragging && hovered ? palette.accentHover
                        : dragging          ? palette.accent
                        : assigned          ? EditorUI::withAlpha(palette.success, 0.7F)
                                            : palette.border;
    draw->AddRectFilled(
        min, max,
        ImGui::GetColorU32(dragging ? EditorUI::withAlpha(palette.accent, hovered ? 0.18F : 0.08F) : palette.surface),
        6.0F);
    // Dashed outline.
    constexpr float dash = 6.0F;
    const ImU32 edgeColor = ImGui::GetColorU32(edge);
    for (float x = min.x + 4.0F; x < max.x - 4.0F; x += dash * 2.0F) {
        draw->AddLine({x, min.y}, {std::min(x + dash, max.x - 4.0F), min.y}, edgeColor);
        draw->AddLine({x, max.y - 1.0F}, {std::min(x + dash, max.x - 4.0F), max.y - 1.0F}, edgeColor);
    }
    for (float y = min.y + 4.0F; y < max.y - 4.0F; y += dash * 2.0F) {
        draw->AddLine({min.x, y}, {min.x, std::min(y + dash, max.y - 4.0F)}, edgeColor);
        draw->AddLine({max.x - 1.0F, y}, {max.x - 1.0F, std::min(y + dash, max.y - 4.0F)}, edgeColor);
    }
    const ImVec4 foreground = assigned ? palette.textPrimary : palette.textSecondary;
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    const float contentWidth = 18.0F + 8.0F + textSize.x;
    const float startX = std::max(min.x + 10.0F, (min.x + max.x - contentWidth) * 0.5F);
    const float centerY = (min.y + max.y) * 0.5F;
    EditorUI::drawIcon(draw, icon, {startX + 9.0F, centerY}, 18.0F,
                       ImGui::GetColorU32(assigned ? palette.success : palette.textSecondary));
    draw->PushClipRect(min, max, true);
    draw->AddText({startX + 26.0F, centerY - textSize.y * 0.5F}, ImGui::GetColorU32(foreground), label);
    draw->PopClipRect();
}

void panelSection(const char* title, const char* description) {
    ImGui::Spacing();
    ImGui::SeparatorText(title);
    if (description != nullptr) {
        ImGui::TextDisabled("%s", description);
    }
    ImGui::Spacing();
}

void fullWidthSlider(const char* label, float* value, const float minimum, const float maximum, const char* format) {
    ImGui::TextDisabled("%s", label);
    ImGui::SetNextItemWidth(-1.0F);
    Editor::Controls::sliderFloat((std::string{"##"} + label).c_str(), value, minimum, maximum, format);
}

void drawSculptSettings(TerrainSculptState& state) {
    panelSection("Sculpt brush", "Shape the terrain heightmap");
    constexpr const char* modes[]{"Raise", "Lower", "Smooth", "Flatten"};
    int mode = static_cast<int>(state.mode);
    ImGui::TextDisabled("Action");
    ImGui::SetNextItemWidth(-1.0F);
    if (ImGui::Combo("##terrain-sculpt-action", &mode, modes, 4))
        state.mode = static_cast<Engine::TerrainSculptMode>(mode);

    fullWidthSlider("Brush size", &state.radius, 0.25F, 8.0F, "%.1f m");
    fullWidthSlider("Strength", &state.strength, 0.1F, 100.0F, "%.1f");

    constexpr const char* falloffs[]{"Smooth", "Linear", "Sharp"};
    int falloff = static_cast<int>(state.falloff);
    ImGui::TextDisabled("Falloff");
    ImGui::SetNextItemWidth(-1.0F);
    if (ImGui::Combo("##terrain-sculpt-falloff", &falloff, falloffs, 3))
        state.falloff = static_cast<Engine::TerrainBrushFalloff>(falloff);
    fullWidthSlider("Spacing", &state.spacing, 0.05F, 1.0F, "%.2f");

    ImGui::Spacing();
    EditorUI::keyChip("Shift");
    ImGui::SameLine();
    ImGui::TextDisabled("Invert raise / lower");
    EditorUI::keyChip("Ctrl");
    ImGui::SameLine();
    ImGui::TextDisabled("Pick flatten height");
}

void drawPaintSettings(Engine::ScenePreset& scene, Engine::Assets::Content& content, const Engine::Entity selected,
                       Engine::Renderer& renderer, TerrainSculptState& state) {
    panelSection("Material paint", "Blend texture layers on the terrain");
    constexpr const char* layers[]{"Layer 1", "Layer 2", "Layer 3", "Base layer"};
    ImGui::TextDisabled("Active layer");
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::Combo("##terrain-paint-layer", &state.paintLayer, layers, 4);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The base layer fills unpainted areas; Layers 1-3 are brush-painted.");

    const auto& terrainRenderer = scene.editor().read<Engine::MeshRenderer>(selected);
    const auto terrainSource = terrainRenderer.mesh.source();
    const bool hasTexture = terrainSource && terrainSource->images.size() > static_cast<std::size_t>(state.paintLayer);
    ImGui::TextDisabled("Layer texture");
    assetDropZone("##terrain-layer-texture", EditorUI::Icon::Image,
                  hasTexture ? "Texture assigned - drop to replace" : "Drop a texture asset here", hasTexture);
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(Editor::AssetDragDrop::texturePayload)) {
            const auto texture = content.texture(Editor::AssetDragDrop::texturePath(*payload));
            if (texture && texture->width > 0 && texture->height > 0 && !texture->rgbaPixels.empty() && terrainSource) {
                auto mesh = std::make_shared<Engine::Mesh>(*terrainSource);
                mesh->images.resize(std::max(mesh->images.size(), static_cast<std::size_t>(state.paintLayer + 1)));
                mesh->images[state.paintLayer] = {texture->width, texture->height, texture->rgbaPixels};
                scene.editor().patch<Engine::TerrainComponent>(selected, [&](auto& terrain) {
                    terrain.materialLayers[state.paintLayer] = mesh->images[state.paintLayer];
                });
                scene.editor().patch<Engine::MeshRenderer>(selected, [&](auto& component) {
                    component.mesh = mesh;
                    for (int layer = 0; layer < 4; ++layer)
                        component.material.pbr.terrainLayerTextures[layer] =
                            layer < static_cast<int>(mesh->images.size()) ? layer : -1;
                    component.material.pbr.terrainLayered = true;
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

void drawDetailSettings(Engine::ScenePreset& scene, Engine::Assets::Content& content, const Engine::Entity selected,
                        TerrainSculptState& state) {
    panelSection("Trees & grass", "Scatter or erase detail meshes");
    constexpr std::array<const char*, 2> brushModes{"Add details", "Erase details"};
    int brushMode = state.grassErase ? 1 : 0;
    if (EditorUI::segmentedControl("##terrain-detail-mode", brushModes, brushMode))
        state.grassErase = brushMode == 1;
    ImGui::Spacing();

    const bool hasDetails = scene.editor().has<Engine::TerrainGrassComponent>(selected) &&
                            scene.editor().read<Engine::TerrainGrassComponent>(selected).hasPrefab();
    std::string prefabLabel = "Drop a tree or grass model here";
    if (hasDetails) {
        const auto& details = scene.editor().read<Engine::TerrainGrassComponent>(selected);
        const std::string name = details.mesh->sourcePath.filename().string();
        prefabLabel = name.empty() ? "Detail mesh assigned" : name;
        ImGui::TextDisabled("%zu placed instances", details.instances.size());
    }
    ImGui::TextDisabled("Detail prefab");
    assetDropZone("##terrain-detail-prefab", EditorUI::Icon::Leaf, prefabLabel.c_str(), hasDetails);
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(Editor::AssetDragDrop::modelPayload)) {
            const auto prefab = Engine::Prefab::model(content, Editor::AssetDragDrop::modelPath(*payload));
            Engine::TerrainGrassComponent component;
            if (scene.editor().has<Engine::TerrainGrassComponent>(selected))
                component = scene.editor().read<Engine::TerrainGrassComponent>(selected);
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
    fullWidthSlider("Maximum scale", &state.grassMaximumScale, state.grassMinimumScale, 4.0F, "%.2f");
    ImGui::Checkbox("Random rotation", &state.grassRandomYaw);
}
} // namespace

void drawTerrainToolsPanel(Engine::ScenePreset& scene, Engine::Assets::Content& content, const Engine::Entity selected,
                           Engine::Renderer& renderer, TerrainSculptState& state, const bool playing, bool& isOpen) {
    ImGui::SetNextWindowSizeConstraints({285.0F, 300.0F},
                                        {std::numeric_limits<float>::max(), std::numeric_limits<float>::max()});
    const bool panelVisible = ImGui::Begin("Terrain Tools", &isOpen);
    if (!isOpen) {
        if (state.strokeActive)
            finishTerrainStroke(scene, state);
        setTerrainTool(state, TerrainToolMode::None);
        ImGui::End();
        return;
    }
    if (!panelVisible) {
        ImGui::End();
        return;
    }

    const bool terrainSelected = !playing && selected != Engine::NullEntity && scene.editor().valid(selected) &&
                                 scene.editor().has<Engine::TerrainComponent>(selected) &&
                                 scene.editor().has<Engine::MeshRenderer>(selected);
    if (!terrainSelected) {
        if (state.strokeActive)
            finishTerrainStroke(scene, state);
        setTerrainTool(state, TerrainToolMode::None);
        ImGui::Dummy({0.0F, ImGui::GetContentRegionAvail().y * 0.15F});
        EditorUI::emptyState(EditorUI::Icon::Terrain, playing ? "Unavailable in Play Mode" : "No terrain selected",
                             playing ? "Stop Play Mode to sculpt, paint or place details."
                                     : "Select a terrain object to start sculpting, painting, or placing details.");
        ImGui::End();
        return;
    }

    const auto& terrain = scene.editor().read<Engine::TerrainComponent>(selected);
    EditorUI::iconLabel(EditorUI::Icon::Terrain, EditorUI::colors().iconTerrain);
    ImGui::SameLine(0.0F, 6.0F);
    ImGui::TextUnformatted(entityName(scene, selected));
    ImGui::TextDisabled("%u x %u heightmap  ·  %.0f x %.0f m", terrain.resolution, terrain.resolution, terrain.width,
                        terrain.depth);
    ImGui::Spacing();

    const TerrainToolMode active = activeTerrainTool(state);
    const float tileSpacing = 4.0F;
    const float tileWidth = std::floor((ImGui::GetContentRegionAvail().x - tileSpacing * 2.0F) / 3.0F);
    if (terrainModeTile("##terrain-sculpt", EditorUI::Icon::Terrain, "Sculpt",
                        "Raise, lower, smooth, and flatten terrain", active == TerrainToolMode::Sculpt, tileWidth)) {
        const TerrainToolMode next =
            active == TerrainToolMode::Sculpt ? TerrainToolMode::None : TerrainToolMode::Sculpt;
        setTerrainTool(state, next);
        auto mesh =
            std::make_shared<Engine::Mesh>(terrain.createMesh(next == TerrainToolMode::Sculpt ? 0U : state.previewLod));
        scene.editor().patch<Engine::MeshRenderer>(selected, [&](auto& component) {
            terrain.applyMaterialLayers(*mesh, component.material.pbr);
            component.mesh = std::move(mesh);
        });
        renderer.synchronizeScene(scene);
    }
    ImGui::SameLine(0.0F, tileSpacing);
    if (terrainModeTile("##terrain-paint", EditorUI::Icon::Brush, "Paint", "Blend up to four terrain texture layers",
                        active == TerrainToolMode::Paint, tileWidth))
        setTerrainTool(state, active == TerrainToolMode::Paint ? TerrainToolMode::None : TerrainToolMode::Paint);
    ImGui::SameLine(0.0F, tileSpacing);
    if (terrainModeTile("##terrain-details", EditorUI::Icon::Leaf, "Details",
                        "Scatter trees, grass and other detail meshes", active == TerrainToolMode::Details, tileWidth))
        setTerrainTool(state, active == TerrainToolMode::Details ? TerrainToolMode::None : TerrainToolMode::Details);

    if (state.enabled)
        drawSculptSettings(state);
    else if (state.paintEnabled)
        drawPaintSettings(scene, content, selected, renderer, state);
    else if (state.grassEnabled)
        drawDetailSettings(scene, content, selected, state);
    else {
        panelSection("Terrain preview", "Display quality while editing");
        int lod = static_cast<int>(state.previewLod);
        ImGui::TextDisabled("Level of detail");
        ImGui::SetNextItemWidth(-1.0F);
        if (Editor::Controls::sliderInt("##terrain-preview-lod", &lod, 0, 5, "%d")) {
            state.previewLod = static_cast<std::uint32_t>(lod);
            auto mesh = std::make_shared<Engine::Mesh>(terrain.createMesh(state.previewLod));
            scene.editor().patch<Engine::MeshRenderer>(selected, [&](auto& component) {
                terrain.applyMaterialLayers(*mesh, component.material.pbr);
                component.mesh = std::move(mesh);
            });
            renderer.synchronizeScene(scene);
        }
        ImGui::TextWrapped("Choose a tool above, then paint directly in the Scene View.");
    }
    ImGui::End();
}
