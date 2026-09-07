#include "Engine/ECS/Components/WindComponent.h"

#include <algorithm>

static bool drawRemovableComponentHeader(const char *label, const char *id, bool &remove) {
    const bool open = ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen);
    // SameLine(pos_x) is relative to the content origin.  Using
    // GetWindowContentRegionMax() here placed the button outside the active
    // item/clip region in the editor after the ImGui backend became shared.
    // Keep the button outside the header's full-width hit rectangle. Drawing
    // it over the header makes ImGui toggle the collapse state instead of
    // delivering the click to the button.
    ImGui::SetCursorPosX(ImGui::GetCursorStartPos().x);
    ImGui::PushID(id);
    remove = ImGui::SmallButton("Remove");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this component");
    ImGui::PopID();
    return open;
}

bool ComponentsPanel::draw(Engine::ScenePreset &scene, const std::vector<Engine::Entity>& selection,
                           const Engine::Entity active, bool& isOpen) {
    const Engine::Entity selected = active;
    ImGui::Begin("Inspector", &isOpen);
    ImGui::TextDisabled("PROPERTIES");
    if (selected != Engine::NullEntity && scene.editor().valid(selected)) {
        ImGui::SameLine();
        ImGui::TextDisabled("/ %s", entityName(scene, selected));
    }
    ImGui::Separator();
    if (selected == Engine::NullEntity || selection.empty()) {
        ImGui::Spacing();
        ImGui::Spacing();
        const float avail = ImGui::GetContentRegionAvail().x;
        ImGui::PushStyleColor(ImGuiCol_Text, {0.42F, 0.68F, 0.92F, 1.0F});
        const char *hintIcon = "◇";
        ImGui::SetCursorPosX((avail - ImGui::CalcTextSize(hintIcon).x) * 0.5F);
        ImGui::TextUnformatted(hintIcon);
        ImGui::PopStyleColor();
        ImGui::Spacing();
        const char *title = "Nothing selected";
        ImGui::SetCursorPosX((avail - ImGui::CalcTextSize(title).x) * 0.5F);
        ImGui::TextDisabled("%s", title);
        ImGui::Spacing();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + avail);
        ImGui::TextWrapped("Pick an object in the Hierarchy or click it in the Scene View to edit its properties here.");
        ImGui::PopTextWrapPos();
        if (!scene.hasUsablePrimaryCamera()) {
            ImGui::Spacing();
            ImGui::TextColored({0.96F, 0.72F, 0.28F, 1.0F},
                               "! Scene problem: no usable primary camera");
            ImGui::TextDisabled("The runtime is rendering with its fallback camera.");
        }
        const bool consumesMouseWheel = ImGui::IsWindowHovered(
                                            ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::GetIO().MouseWheel
                                        != 0.0F;
        ImGui::End();
        return consumesMouseWheel;
    }

    // Every inspector control has a stable label, so scope their ImGui IDs to
    // the selected entity. Without this, an active text/drag control from the
    // source object is reused when a duplicate becomes selected in the same
    // panel, making the inspector appear to keep editing the original.
    ImGui::PushID(reinterpret_cast<const void *>(static_cast<std::uintptr_t>(selected)));
    ImGui::TextColored({0.94F, 0.95F, 0.98F, 1.0F}, "%s", entityName(scene, selected));
    ImGui::SameLine();
    ImGui::TextDisabled("· Entity %u", Engine::entityIndex(selected));
    const bool multiSelection = selection.size() > 1;
    if (multiSelection) ImGui::TextDisabled("%zu objects selected · editing common Transform", selection.size());
    if (!multiSelection && scene.editor().valid(selected) && scene.editor().has<Engine::NameComponent>(selected)) {
        const auto readScene = scene.editor();
        const auto &name = readScene.read<Engine::NameComponent>(selected).value;
        char editableName[260]{};
        std::snprintf(editableName, sizeof(editableName), "%s", name.c_str());
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::InputTextWithHint("##object-name", "Object name", editableName, sizeof(editableName)) &&
            editableName[0] != '\0') {
            try {
                scene.editor().rename(selected, editableName);
            } catch (const std::invalid_argument&) {
                // Keep the previous name until a unique, non-empty value is entered.
            }
        }
    }
    ImGui::TextDisabled("Rename the object, then tweak its components below.");
    if (!scene.hasUsablePrimaryCamera()) {
        ImGui::Spacing();
        ImGui::TextColored({0.96F, 0.72F, 0.28F, 1.0F},
                           "! Scene problem: no usable primary camera");
        ImGui::TextDisabled("The runtime is rendering with its fallback camera.");
        ImGui::TextDisabled("Add or repair a perspective camera with Transform, then mark it Primary.");
    }
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen) &&
        scene.editor().valid(selected) && scene.editor().has<Engine::Transform>(selected)) {
        if (!multiSelection) {
            TransformFields{scene.edit(selected)}.draw();
            if (scene.editor().has<Engine::ParentComponent>(selected)) {
                ImGui::Spacing();
                if (ImGui::Button("Reset to Parent")) {
                    scene.editor().patch<Engine::Transform>(selected, [](auto& transform) {
                        transform.position = {0.0F, 0.0F, 0.0F};
                        transform.rotation = {0.0F, 0.0F, 0.0F};
                        transform.scale = {1.0F, 1.0F, 1.0F};
                    });
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Set local position and rotation to zero, and local scale to one.");
                }
            }
        } else {
            const auto transform = scene.editor().read<Engine::Transform>(selected);
            const auto apply = [&](const auto setter, const Engine::Vec3& value) {
                for (const Engine::Entity entity : selection) {
                    if (scene.editor().valid(entity) && scene.editor().has<Engine::Transform>(entity)) {
                        (scene.edit(entity).*setter)(value);
                    }
                }
            };
            ImGui::TextDisabled("Values apply to every selected object.");
            EditableField::drawSharedVec3Field("Position", "##multi-position", transform.position, 0.05F, "%.2F",
                [&](const Engine::Vec3& value) { apply(&Engine::GameObject::setPosition, value); });
            EditableField::drawSharedVec3Field("Rotation", "##multi-rotation", transform.rotation, 0.5F, "%.1F°",
                [&](const Engine::Vec3& value) { apply(&Engine::GameObject::setRotation, value); });
            EditableField::drawSharedVec3Field("Scale", "##multi-scale", transform.scale, 0.01F, "%.2F",
                [&](const Engine::Vec3& value) { apply(&Engine::GameObject::setScale, value); });
        }
    }
    if (multiSelection) {
        ImGui::Separator();
        ImGui::TextDisabled("Other components are shown for one object at a time.");
        const bool consumesMouseWheel = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
            ImGui::GetIO().MouseWheel != 0.0F;
        ImGui::PopID();
        ImGui::End();
        return consumesMouseWheel;
    }
    if (scene.editor().valid(selected) && scene.editor().has<Engine::MeshRenderer>(selected)) {
        bool remove = false;
        const bool open = drawRemovableComponentHeader("Mesh Renderer", "mesh-renderer", remove);
        if (remove) {
            scene.editor().remove<Engine::MeshRenderer>(selected);
        } else if (open) {
        const auto readScene = scene.editor();
        const auto &source = readScene.read<Engine::MeshRenderer>(selected);
        auto renderer = source;
        bool changed = false;

        ImGui::TextDisabled("Mesh");
        if (renderer.mesh && !renderer.mesh->empty()) {
            const auto path = renderer.mesh->sourcePath.generic_string();
            ImGui::TextWrapped("%s", path.empty() ? "Generated geometry" : path.c_str());
            ImGui::TextDisabled("%u vertices · %u triangles · %zu textures",
                                renderer.mesh->vertexCount(), renderer.mesh->indexCount() / 3,
                                renderer.mesh->images.size());
            ImGui::TextDisabled("%zu source materials", renderer.mesh->materials.size());
        } else {
            ImGui::TextColored({0.95F, 0.40F, 0.35F, 1.0F}, "Missing mesh");
        }

        ImGui::Separator();
        ImGui::TextDisabled("Material override");
        if (ImGui::Checkbox("Override imported material slot 0##mesh-material", &renderer.materialOverride)) {
            if (renderer.materialOverride && renderer.mesh && !renderer.mesh->materials.empty())
                renderer.material = renderer.mesh->materials.front();
            changed = true;
        }
        float baseColor[4] = {
            renderer.material.baseColor.r(), renderer.material.baseColor.g(),
            renderer.material.baseColor.b(), renderer.material.baseColor.a()
        };
        if (ImGui::ColorEdit4("Base Color##mesh-material", baseColor,
                              ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_Float)) {
            renderer.material.baseColor = Engine::Color{baseColor[0], baseColor[1],
                                                         baseColor[2], baseColor[3]}.clamped();
            changed = true;
        }
        changed |= Editor::Controls::sliderFloat("Metallic##mesh-material", &renderer.material.metallic,
                                      0.0F, 1.0F, "%.2f");
        changed |= Editor::Controls::sliderFloat("Roughness##mesh-material", &renderer.material.roughness,
                                      0.0F, 1.0F, "%.2f");
        changed |= Editor::Controls::sliderFloat("Ambient Occlusion##mesh-material",
                                      &renderer.material.aoStrength, 0.0F, 1.0F, "%.2f");
        changed |= ImGui::DragFloat("Normal Scale##mesh-material", &renderer.material.normalScale,
                                    0.01F, 0.0F, 10.0F, "%.2f");
        ImGui::Separator();
        ImGui::TextDisabled("Emission");
        float emissive[3] = {renderer.material.emissiveColor.r(), renderer.material.emissiveColor.g(),
                             renderer.material.emissiveColor.b()};
        if (ImGui::ColorEdit3("Emissive Color##mesh-material", emissive, ImGuiColorEditFlags_Float)) {
            renderer.material.emissiveColor = Engine::Color{emissive[0], emissive[1], emissive[2]}.clamped();
            changed = true;
        }
        changed |= ImGui::DragFloat("Emissive Intensity##mesh-material", &renderer.material.emissiveIntensity,
                                    0.1F, 0.0F, 1000.0F, "%.2f");
        renderer.material.metallic = std::clamp(renderer.material.metallic, 0.0F, 1.0F);
        renderer.material.roughness = std::clamp(renderer.material.roughness, 0.0F, 1.0F);
        renderer.material.aoStrength = std::clamp(renderer.material.aoStrength, 0.0F, 1.0F);
        renderer.material.normalScale = std::max(0.0F, renderer.material.normalScale);
        renderer.material.emissiveIntensity = std::max(0.0F, renderer.material.emissiveIntensity);

        int alphaMode = static_cast<int>(renderer.material.alphaMode);
        constexpr const char* alphaModes[] = {"Opaque", "Mask", "Blend"};
        if (ImGui::Combo("Alpha Mode##mesh-material", &alphaMode, alphaModes, 3)) {
            renderer.material.alphaMode = static_cast<Engine::AlphaMode>(alphaMode);
            changed = true;
        }
        changed |= ImGui::Checkbox("Double Sided##mesh-material", &renderer.material.doubleSided);
        if (renderer.material.alphaMode == Engine::AlphaMode::Mask) {
            changed |= Editor::Controls::sliderFloat("Alpha Cutoff##mesh-material", &renderer.material.alphaCutoff,
                                          0.0F, 1.0F, "%.2f");
            renderer.material.alphaCutoff = std::clamp(renderer.material.alphaCutoff, 0.0F, 1.0F);
        }

        ImGui::Separator();
        ImGui::TextDisabled("Rendering");
        changed |= ImGui::Checkbox("Cast Shadows##mesh-renderer", &renderer.castShadow);
        int cullingBatch = static_cast<int>(renderer.cullingBatch);
        if (ImGui::DragInt("Culling Batch##mesh-renderer", &cullingBatch, 1.0F, 0, 0,
                           "%d")) {
            renderer.cullingBatch = static_cast<std::uint32_t>(std::max(0, cullingBatch));
            changed = true;
        }

        if (changed) {
            scene.editor().patch<Engine::MeshRenderer>(selected,
                [&](auto &component) { component = renderer; });
        }
        }
    }
    if (scene.editor().valid(selected) && scene.editor().has<Engine::TerrainComponent>(selected)) {
        bool remove = false;
        const bool open = drawRemovableComponentHeader("Terrain", "terrain", remove);
        if (remove) {
            scene.editor().remove<Engine::TerrainComponent>(selected);
        } else if (open) {
        const auto& terrain = scene.editor().read<Engine::TerrainComponent>(selected);
        ImGui::Text("Heightmap: %u x %u", terrain.resolution, terrain.resolution);
        ImGui::Text("Size: %.1f x %.1f", terrain.width, terrain.depth);
        ImGui::Text("Height range: %.1f to %.1f", terrain.minimumHeight, terrain.maximumHeight);
        ImGui::Spacing();
        ImGui::TextWrapped("Use Sculpt in the Scene View toolbar, then drag the left mouse button over the terrain.");
        if (scene.editor().has<Engine::TerrainGrassComponent>(selected)) {
            const auto& grass = scene.editor().read<Engine::TerrainGrassComponent>(selected);
            ImGui::Text("Grass instances: %zu", grass.instances.size());
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove##terrain-grass")) {
                scene.editor().remove<Engine::TerrainGrassComponent>(selected);
            } else {
            bool castGrassShadow = grass.castShadow;
            if (ImGui::Checkbox("Cast Shadows##terrain-grass", &castGrassShadow)) {
                scene.editor().patch<Engine::TerrainGrassComponent>(selected,
                    [&](auto& component) { component.castShadow = castGrassShadow; });
            }
            if (!grass.instances.empty() && ImGui::Button("Clear grass")) {
                auto cleared = grass;
                cleared.instances.clear();
                scene.editor().remove<Engine::TerrainGrassComponent>(selected);
                scene.editor().add<Engine::TerrainGrassComponent>(selected, std::move(cleared));
            }
            ImGui::SameLine();
            ImGui::TextDisabled("GPU instanced virtual shadows");
            if (!grass.instances.empty() && ImGui::Button("Reset trampled grass")) {
                scene.editor().patch<Engine::TerrainGrassComponent>(selected, [](auto& component) {
                    for (auto& instance : component.instances) {
                        instance.bendX = 0.0F;
                        instance.bendZ = 0.0F;
                        instance.trampled = 0.0F;
                    }
                    component.allInstancesDirty = true;
                });
            }
            }
        } else {
            ImGui::TextDisabled("Grass: choose Grass in Scene View and drop a model prefab.");
        }
        }
    }
    if (scene.editor().valid(selected) &&
        scene.editor().has<Engine::LightComponent>(selected)) {
        bool remove = false;
        const bool open = drawRemovableComponentHeader("Light", "light", remove);
        if (remove) {
            scene.editor().remove<Engine::LightComponent>(selected);
        } else if (open) {
            const auto source = scene.editor().read<Engine::LightComponent>(selected);
            auto light = source;
            constexpr const char *typeNames[] = {"Directional", "Point", "Spot"};
            int type = static_cast<int>(light.type);
            bool changed = false;

            ImGui::SetNextItemWidth(-1.0F);
            if (ImGui::BeginCombo("Type##light", typeNames[type])) {
                for (int index = 0; index < std::size(typeNames); ++index) {
                    if (ImGui::Selectable(typeNames[index], type == index)) {
                        type = index;
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            light.type = static_cast<Engine::LightType>(type);

            float color[3] = {light.color.r(), light.color.g(), light.color.b()};
            if (ImGui::ColorEdit3("Color##light", color, ImGuiColorEditFlags_Float)) {
                light.color = Engine::Color{color[0], color[1], color[2]}.clamped();
                changed = true;
            }
            changed |= ImGui::DragFloat("Intensity##light", &light.intensity, 0.05F,
                                        0.0F, 1000.0F, "%.2f");
            if (light.type == Engine::LightType::Directional) {
                const bool wasEnabled = light.enabled;
                if (ImGui::Checkbox("Active Directional Light##light", &light.enabled)) {
                    changed = true;
                    if (light.enabled && !wasEnabled) {
                        scene.editor().setActiveDirectionalLight(selected);
                    }
                }
                ImGui::TextDisabled("Only one directional light can be active.");
            } else {
                changed |= ImGui::DragFloat("Range##light", &light.range, 0.1F, 0.1F,
                                            1000.0F, "%.2f");
                if (light.type == Engine::LightType::Spot) {
                    changed |= ImGui::DragFloat("Inner cone##light", &light.innerConeAngle,
                                                0.25F, 0.0F, 89.0F, "%.1f deg");
                    changed |= ImGui::DragFloat("Outer cone##light", &light.outerConeAngle,
                                                0.25F, light.innerConeAngle, 89.9F, "%.1f deg");
                }
            }
            changed |= ImGui::Checkbox("Cast Shadows##light", &light.castShadows);
            light.intensity = std::max(0.0F, light.intensity);
            light.range = std::max(0.1F, light.range);
            light.innerConeAngle = std::clamp(light.innerConeAngle, 0.0F, 89.0F);
            light.outerConeAngle = std::clamp(light.outerConeAngle, light.innerConeAngle + 0.1F,
                                               89.9F);

            if (changed) {
                scene.editor().patch<Engine::LightComponent>(selected,
                    [&](auto &component) { component = light; });
            }
        }
    }

    if (scene.editor().valid(selected) && scene.editor().has<Engine::WindComponent>(selected)) {
        bool remove = false;
        const bool open = drawRemovableComponentHeader("Wind", "wind", remove);
        if (remove) {
            scene.editor().remove<Engine::WindComponent>(selected);
        } else if (open) {
            auto wind = scene.editor().read<Engine::WindComponent>(selected);
            float direction[3] = {wind.direction.x(), wind.direction.y(), wind.direction.z()};
            bool changed = ImGui::Checkbox("Enabled##wind", &wind.enabled);
            changed |= ImGui::DragFloat3("Direction##wind", direction, 0.02F, -1.0F, 1.0F);
            changed |= ImGui::SliderFloat("Strength##wind", &wind.strength, 0.0F, 1.0F);
            changed |= ImGui::SliderFloat("Gust strength##wind", &wind.gustStrength, 0.0F, 0.5F);
            changed |= ImGui::SliderFloat("Frequency##wind", &wind.frequency, 0.0F, 4.0F);
            changed |= ImGui::DragFloat("Range##wind", &wind.range, 0.1F, 0.1F, 500.0F, "%.1f m");
            const Engine::Vec3 candidate{direction[0], direction[1], direction[2]};
            if (candidate.length() <= 1.0e-4F) {
                ImGui::TextDisabled("Direction must be non-zero");
            } else if (changed) {
                wind.direction = candidate;
                scene.editor().patch<Engine::WindComponent>(selected,
                    [&](auto& component) { component = wind; });
            }
        }
    }
    if (scene.editor().valid(selected) &&
        scene.editor().has<Engine::SmokeEmitterComponent>(selected)) {
        bool remove = false;
        const bool open = drawRemovableComponentHeader("Smoke Emitter", "smoke-emitter", remove);
        if (remove) {
            scene.editor().remove<Engine::SmokeEmitterComponent>(selected);
        } else if (open) {
        // Keep the UI editing a temporary copy. The component is committed
        // once, after all controls have been drawn, so observers receive one
        // coherent change notification per frame.
        const auto readScene = scene.editor();
        const auto &source =
                readScene.read<Engine::SmokeEmitterComponent>(selected).emitter;
        auto emitter = source;

        bool changed = false;
        const auto drawParticleFloat = [](const char *label, const char *id,
                                          float *value, const float speed,
                                          const float min, const float max,
                                          const char *format) {
            ImGui::TextDisabled("%s", label);
            ImGui::SetNextItemWidth(-1.0F);
            return ImGui::DragFloat(id, value, speed, min, max, format);
        };

        changed |= drawParticleFloat("Spawn Rate", "##particle-spawn-rate",
                                     &emitter.spawnRate, 1.0F, 0.0F, 5000.0F,
                                     "%.0F particles/s");
        changed |= drawParticleFloat("Minimum Lifetime", "##particle-min-lifetime",
                                     &emitter.minLifeTime, 0.01F, 0.0F, 60.0F,
                                     "%.2F s");
        changed |= drawParticleFloat("Maximum Lifetime", "##particle-max-lifetime",
                                     &emitter.maxLifeTime, 0.01F, 0.0F, 60.0F,
                                     "%.2F s");
        changed |= drawParticleFloat("Minimum Size", "##particle-min-size",
                                     &emitter.minSize, 0.01F, 0.0F, 10.0F,
                                     "%.2F");
        changed |= drawParticleFloat("Maximum Size", "##particle-max-size",
                                     &emitter.maxSize, 0.01F, 0.0F, 10.0F,
                                     "%.2F");
        changed |= drawParticleFloat("Buoyancy", "##smoke-buoyancy",
                                     &emitter.buoyancy, 0.05F, 0.0F, 30.0F,
                                     "%.2F");
        changed |= drawParticleFloat("Air Drag", "##smoke-drag",
                                     &emitter.drag, 0.02F, 0.0F, 10.0F,
                                     "%.2F");
        changed |= drawParticleFloat("Turbulence", "##smoke-turbulence",
                                     &emitter.turbulence, 0.02F, 0.0F, 10.0F,
                                     "%.2F");
        changed |= drawParticleFloat("Collision Radius", "##smoke-collision-radius",
                                     &emitter.collisionRadius, 0.005F, 0.0F, 2.0F,
                                     "%.3F");

        float minVelocity[3] = {
            emitter.minVelocity.x(), emitter.minVelocity.y(), emitter.minVelocity.z()
        };
        float maxVelocity[3] = {
            emitter.maxVelocity.x(), emitter.maxVelocity.y(), emitter.maxVelocity.z()
        };
        if (ImGui::DragFloat3("Min Velocity", minVelocity, 0.05F, -100.0F, 100.0F)) {
            emitter.minVelocity = {minVelocity[0], minVelocity[1], minVelocity[2]};
            changed = true;
        }
        if (ImGui::DragFloat3("Max Velocity", maxVelocity, 0.05F, -100.0F, 100.0F)) {
            emitter.maxVelocity = {maxVelocity[0], maxVelocity[1], maxVelocity[2]};
            changed = true;
        }

        float color[4] = {
            emitter.color.r(), emitter.color.g(), emitter.color.b(), emitter.color.a()
        };
        ImGui::TextDisabled("Color");
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::ColorEdit4("##particle-color", color, ImGuiColorEditFlags_AlphaBar)) {
            emitter.color = Engine::Color{color[0], color[1], color[2], color[3]};
            changed = true;
        }

        // Enforce valid ranges even when values are entered from the keyboard.
        emitter.minLifeTime = std::max(0.0F, emitter.minLifeTime);
        emitter.maxLifeTime = std::max(emitter.minLifeTime, emitter.maxLifeTime);
        emitter.minSize = std::max(0.0F, emitter.minSize);
        emitter.maxSize = std::max(emitter.minSize, emitter.maxSize);
        emitter.spawnRate = std::max(0.0F, emitter.spawnRate);
        emitter.buoyancy = std::max(0.0F, emitter.buoyancy);
        emitter.drag = std::max(0.0F, emitter.drag);
        emitter.turbulence = std::max(0.0F, emitter.turbulence);
        emitter.collisionRadius = std::max(0.0F, emitter.collisionRadius);

        if (changed) {
            scene.editor().patch<Engine::SmokeEmitterComponent>(selected,
                                                                 [&](auto &component) {
                                                                     component.emitter = emitter;
                                                                 });
        }
        }
    }
    if (scene.editor().valid(selected) &&
        scene.editor().has<Engine::ProceduralCloudComponent>(selected)) {
        bool remove = false;
        const bool open = drawRemovableComponentHeader("Procedural Cloud", "procedural-cloud", remove);
        if (remove) {
            scene.editor().remove<Engine::ProceduralCloudComponent>(selected);
            // Procedural Cloud creates its renderable mesh as a companion
            // component; remove that generated renderer as part of the same
            // user-facing operation.
            if (scene.editor().has<Engine::MeshRenderer>(selected)) {
                scene.editor().remove<Engine::MeshRenderer>(selected);
            }
        } else if (open) {
            const auto readScene = scene.editor();
            auto cloud = readScene.read<Engine::ProceduralCloudComponent>(selected);
            bool changed = false;
            int seed = static_cast<int>(cloud.seed);
            int puffCount = static_cast<int>(cloud.puffCount);
            changed |= ImGui::DragInt("Seed", &seed, 1.0F, 1, 2'000'000'000);
            changed |= ImGui::SliderInt("Puff Count", &puffCount, 1, 128);
            float dimensions[3] = {cloud.dimensions.x(), cloud.dimensions.y(), cloud.dimensions.z()};
            changed |= ImGui::DragFloat3("Dimensions", dimensions, 0.1F, 0.1F, 200.0F, "%.1f");
            changed |= ImGui::DragFloat("Puff Radius", &cloud.puffRadius, 0.02F, 0.05F, 20.0F, "%.2f");
            ImGui::TextDisabled("Regenerated deterministically after each change.");
            if (changed) {
                cloud.seed = static_cast<std::uint32_t>(std::max(seed, 1));
                cloud.puffCount = static_cast<std::uint32_t>(std::clamp(puffCount, 1, 128));
                cloud.dimensions = {std::max(dimensions[0], 0.1F), std::max(dimensions[1], 0.1F),
                                    std::max(dimensions[2], 0.1F)};
                cloud.puffRadius = std::max(cloud.puffRadius, 0.05F);
                scene.editor().patch<Engine::ProceduralCloudComponent>(selected,
                    [&](auto& component) { component = cloud; });
                scene.editor().patch<Engine::MeshRenderer>(selected, [&](auto& renderer) {
                    renderer.mesh = std::make_shared<Engine::Mesh>(Engine::ProceduralCloud::createMesh(cloud));
                });
            }
        }
    }
    if (scene.editor().valid(selected) && scene.editor().has<Engine::ScriptComponent>(selected)) {
        bool remove = false;
        const bool open = drawRemovableComponentHeader("Script", "script", remove);
        if (remove) {
            scene.editor().remove<Engine::ScriptComponent>(selected);
        } else if (open) {
        const auto readScene = scene.editor();
        const auto &script = readScene.read<Engine::ScriptComponent>(selected);
        char className[260]{};
        std::snprintf(className, sizeof(className), "%s", script.className.c_str());
        ImGui::TextDisabled("C++ script class");
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::InputText("##script-class", className, sizeof(className))) {
            scene.editor().patch<Engine::ScriptComponent>(selected, [&](auto &value) {
                value.className = className;
                value.reset();
            });
        }
        bool enabled = script.enabled;
        if (ImGui::Checkbox("Enabled##script", &enabled)) {
            scene.editor().patch<Engine::ScriptComponent>(selected, [&](auto &value) {
                value.enabled = enabled;
            });
        }
        }
    }
    if (scene.editor().valid(selected) && scene.editor().has<Engine::ColliderComponent>(selected)) {
        bool remove = false;
        const bool open = drawRemovableComponentHeader("Collider", "collider", remove);
        if (remove) {
            scene.editor().remove<Engine::ColliderComponent>(selected);
        } else if (open) {
            const auto collider = scene.editor().read<Engine::ColliderComponent>(selected);
            int shape = static_cast<int>(collider.shape.index());
            const char *shapeNames[] = {"Box", "Sphere", "Capsule", "Ramp", "Mesh"};
            const bool hasMesh = scene.editor().has<Engine::MeshRenderer>(selected) &&
                scene.editor().read<Engine::MeshRenderer>(selected).hasMesh();
            const bool hasBody = scene.editor().has<Engine::RigidbodyComponent>(selected);
            const Engine::RigidbodyType bodyType = hasBody
                ? scene.editor().read<Engine::RigidbodyComponent>(selected).type
                : Engine::RigidbodyType::Static;
            bool changed = false;

            ImGui::TextDisabled("Geometry");
            ImGui::SetNextItemWidth(-1.0F);
            if (ImGui::BeginCombo("Shape##collider", shapeNames[shape])) {
                for (int index = 0; index < 5; ++index) {
                    const bool unavailable = index == 4 && !hasMesh;
                    if (unavailable) ImGui::BeginDisabled();
                    if (ImGui::Selectable(shapeNames[index], shape == index)) {
                        shape = index;
                        changed = true;
                    }
                    if (unavailable) ImGui::EndDisabled();
                }
                ImGui::EndCombo();
            }

            auto value = collider;
            if (shape != static_cast<int>(value.shape.index())) {
                value.shape = shape == 0
                                  ? Engine::ColliderShape{Engine::BoxCollider{}}
                                  : shape == 1
                                        ? Engine::ColliderShape{Engine::SphereCollider{}}
                                        : shape == 2
                                              ? Engine::ColliderShape{Engine::CapsuleCollider{}}
                                              : shape == 3
                                                    ? Engine::ColliderShape{Engine::RampCollider{}}
                                                    : Engine::ColliderShape{Engine::MeshCollider{
                                                        scene.editor().read<Engine::MeshRenderer>(selected).mesh}};
            }

            ImGui::TextDisabled("Local offset");
            float offset[3] = {value.offset.x(), value.offset.y(), value.offset.z()};
            ImGui::SetNextItemWidth(-1.0F);
            if (ImGui::DragFloat3("Offset##collider", offset, 0.05F)) {
                value.offset = {offset[0], offset[1], offset[2]};
                changed = true;
            }
            std::visit([&]<typename T>(T &colliderShape) {
                using Shape = std::decay_t<T>;
                if constexpr (std::is_same_v<Shape, Engine::BoxCollider> ||
                              std::is_same_v<Shape, Engine::RampCollider>) {
                    float extents[3] = {
                        colliderShape.halfExtents.x(), colliderShape.halfExtents.y(),
                        colliderShape.halfExtents.z()};
                    ImGui::TextDisabled("Half extents");
                    ImGui::SetNextItemWidth(-1.0F);
                    if (ImGui::DragFloat3("##collider-half-extents", extents, 0.05F,
                                          0.001F, 1000.0F)) {
                        colliderShape.halfExtents = {
                            std::max(0.001F, extents[0]), std::max(0.001F, extents[1]),
                            std::max(0.001F, extents[2])};
                        changed = true;
                    }
                    if constexpr (std::is_same_v<Shape, Engine::RampCollider>) {
                        ImGui::TextDisabled("Slope rises along local +Z.");
                    }
                } else if constexpr (std::is_same_v<Shape, Engine::SphereCollider>) {
                    changed |= ImGui::DragFloat("Radius##collider", &colliderShape.radius,
                                                0.05F, 0.001F, 1000.0F);
                    colliderShape.radius = std::max(0.001F, colliderShape.radius);
                } else if constexpr (std::is_same_v<Shape, Engine::CapsuleCollider>) {
                    changed |= ImGui::DragFloat("Radius##collider", &colliderShape.radius,
                                                0.05F, 0.001F, 1000.0F);
                    changed |= ImGui::DragFloat("Total Height##collider", &colliderShape.height,
                                                0.05F, 0.001F, 1000.0F);
                    colliderShape.radius = std::max(0.001F, colliderShape.radius);
                    colliderShape.height = std::max(colliderShape.radius * 2.0F,
                                                    colliderShape.height);
                } else if constexpr (std::is_same_v<Shape, Engine::MeshCollider>) {
                    const char *mode = bodyType == Engine::RigidbodyType::Dynamic
                        ? "Dynamic body: convex hull (bounds fallback)."
                        : "Static body: exact triangle mesh.";
                    ImGui::TextDisabled("%s", mode);
                }
            }, value.shape);

            ImGui::Separator();
            ImGui::TextDisabled("Simulation");
            changed |= ImGui::Checkbox("Trigger (query only)##collider", &value.isTrigger);
            if (value.isTrigger) {
                ImGui::TextDisabled("Triggers receive queries but do not block bodies.");
            }

            ImGui::Separator();
            ImGui::TextDisabled("Material");
            changed |= ImGui::DragFloat("Static / dynamic friction##collider", &value.friction,
                                        0.01F, 0.0F, 10.0F, "%.2f");
            changed |= Editor::Controls::sliderFloat("Restitution##collider", &value.restitution,
                                          0.0F, 1.0F, "%.2f");
            value.friction = std::max(0.0F, value.friction);
            value.restitution = std::clamp(value.restitution, 0.0F, 1.0F);
            if (changed) {
                scene.editor().patch<Engine::ColliderComponent>(selected,
                    [&](auto &component) { component = value; });
            }
        }
    }
    if (scene.editor().valid(selected) && scene.editor().has<Engine::RigidbodyComponent>(selected)) {
        bool remove = false;
        const bool open = drawRemovableComponentHeader("Rigidbody", "rigidbody", remove);
        if (remove) {
            scene.editor().remove<Engine::RigidbodyComponent>(selected);
        } else if (open) {
            const auto rigidbody = scene.editor().read<Engine::RigidbodyComponent>(selected);
            auto value = rigidbody;
            int type = static_cast<int>(value.type);
            const char *typeNames[] = {"Static", "Dynamic", "Kinematic"};
            ImGui::TextDisabled("Body type");
            ImGui::SetNextItemWidth(-1.0F);
            if (ImGui::BeginCombo("Type##rigidbody", typeNames[type])) {
                for (int index = 0; index < 3; ++index) {
                    if (ImGui::Selectable(typeNames[index], type == index)) type = index;
                }
                ImGui::EndCombo();
            }
            value.type = static_cast<Engine::RigidbodyType>(type);
            bool changed = value.type != rigidbody.type;
            const bool dynamic = value.type == Engine::RigidbodyType::Dynamic;
            const bool kinematic = value.type == Engine::RigidbodyType::Kinematic;
            ImGui::TextDisabled(dynamic ? "Simulated by the physics system." :
                                kinematic ? "Driven by Transform; pushes dynamic bodies." :
                                            "Fixed collision geometry.");

            ImGui::Separator();
            ImGui::TextDisabled("Motion");
            if (!dynamic) ImGui::BeginDisabled();
            changed |= ImGui::Checkbox("Use Gravity##rigidbody", &value.useGravity);
            changed |= ImGui::DragFloat("Mass##rigidbody", &value.mass, 0.05F, 0.001F,
                                        100000.0F, "%.3f kg");
            changed |= ImGui::DragFloat("Linear Damping##rigidbody", &value.linearDamping,
                                        0.01F, 0.0F, 100.0F, "%.3f");
            changed |= ImGui::DragFloat("Angular Damping##rigidbody", &value.angularDamping,
                                        0.01F, 0.0F, 100.0F, "%.3f");
            changed |= ImGui::Checkbox("Lock Rotation##rigidbody", &value.fixedRotation);
            if (!dynamic) ImGui::EndDisabled();

            value.mass = std::max(0.001F, value.mass);
            value.linearDamping = std::max(0.0F, value.linearDamping);
            value.angularDamping = std::max(0.0F, value.angularDamping);
            if (changed) {
                scene.editor().patch<Engine::RigidbodyComponent>(selected,
                    [&](auto &component) { component = value; });
            }
        }
    }
    ImGui::TextDisabled("COMPONENTS");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::PushStyleColor(ImGuiCol_Button, {0.20F, 0.36F, 0.52F, 1.0F});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.28F, 0.48F, 0.68F, 1.0F});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.16F, 0.30F, 0.44F, 1.0F});
    if (EditorButton("+  Add Component", {-1.0F, 0.0F}).draw()) {
        ImGui::OpenPopup("Add Component");
    }
    ImGui::PopStyleColor(3);
    if (ImGui::BeginPopup("Add Component")) {
        // The popup deliberately knows nothing about concrete ECS types. New
        // components appear here solely by registering a descriptor.
        static char componentSearch[128]{};
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputTextWithHint("##component-search", "Search components...", componentSearch,
                                 sizeof(componentSearch));
        ImGui::Separator();

        std::string_view previousCategory;
        bool found = false;
        for (const Editor::ComponentDescriptor& descriptor :
             Editor::ComponentRegistry::instance().components()) {
            if (!containsCaseInsensitive(descriptor.name.data(), componentSearch) &&
                !containsCaseInsensitive(descriptor.category.data(), componentSearch)) {
                continue;
            }
            found = true;
            if (previousCategory != descriptor.category) {
                if (!previousCategory.empty()) ImGui::Spacing();
                ImGui::TextDisabled("%.*s", static_cast<int>(descriptor.category.size()), descriptor.category.data());
                previousCategory = descriptor.category;
            }
            const bool available = descriptor.canAdd && descriptor.canAdd(scene, selected);
            if (ImGui::MenuItem(descriptor.name.data(), nullptr, false, available)) {
                descriptor.add(scene, selected);
                componentSearch[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%.*s%s", static_cast<int>(descriptor.description.size()), descriptor.description.data(),
                                  available ? "" : descriptor.singleton ? "\nOnly one instance is allowed in a scene."
                                                                     : "\nAlready added to this object.");
            }
        }
        if (!found) ImGui::TextDisabled("No matching components.");
        ImGui::EndPopup();
    }
    if (EditorButton("Add Script", {-1.0F, 0.0F}).draw()) ImGui::OpenPopup("Add Script");
    if (ImGui::BeginPopupModal("Add Script", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static char scriptSearch[128]{};
        ImGui::TextUnformatted("Choose a registered C++ script.");
        ImGui::SetNextItemWidth(360.0F);
        ImGui::InputTextWithHint("##script-search", "Search scripts...", scriptSearch, sizeof(scriptSearch));
        ImGui::Separator();
        bool found = false;
        for (const std::string& className : Engine::ScriptRegistry::instance().classNames()) {
            if (!containsCaseInsensitive(className.c_str(), scriptSearch)) continue;
            found = true;
            if (ImGui::Selectable(className.c_str())) {
                if (!scene.editor().has<Engine::ScriptComponent>(selected)) {
                    scene.editor().add<Engine::ScriptComponent>(selected);
                }
                scene.editor().patch<Engine::ScriptComponent>(selected, [&](auto& script) {
                    script.className = className;
                    script.enabled = true;
                    script.reset();
                });
                scriptSearch[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
        }
        if (!found) ImGui::TextDisabled("No registered scripts found.");
        ImGui::Separator();
        if (EditorButton("+ New C++ Script", {-1.0F, 0.0F}).draw()) {
            scriptSearch[0] = '\0';
            ImGui::CloseCurrentPopup();
            ImGui::OpenPopup("Create C++ Script");
        }
        if (EditorButton("Cancel", {-1.0F, 0.0F}).draw()) {
            scriptSearch[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (EditorButton("New C++ Script", {-1.0F, 0.0F}).draw()) ImGui::OpenPopup("Create C++ Script");
    if (ImGui::BeginPopupModal("Create C++ Script", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static char name[128]{};
        static std::string error;
        ImGui::TextUnformatted("Creates Assets/Scripts/<Name>.h and .cpp in the current project");
        ImGui::InputTextWithHint("Class name", "PlayerController", name, sizeof(name));
        if (!error.empty()) ImGui::TextColored({1, .3F, .3F, 1}, "%s", error.c_str());
        if (EditorButton("Create").draw() && EditorSceneSession::createCppScript(name, error)) {
            if (!scene.editor().has<Engine::ScriptComponent>(selected))
                scene.editor().add<
                    Engine::ScriptComponent>(selected);
            scene.editor().patch<Engine::ScriptComponent>(selected, [&](auto &script) {
                script.className = name;
                script.reset();
            });
            name[0] = '\0';
            error.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (EditorButton("Cancel").draw()) {
            name[0] = '\0';
            error.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    const bool consumesMouseWheel = ImGui::IsWindowHovered(
                                        ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::GetIO().MouseWheel !=
                                    0.0F;
    ImGui::End();
    return consumesMouseWheel;
}
#include "Engine/ECS/Components/WindComponent.h"
