static bool drawRemovableComponentHeader(const char *label, const char *id, bool &remove) {
    const bool open = ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize("Remove").x -
                    ImGui::GetStyle().FramePadding.x * 2.0F);
    ImGui::PushID(id);
    remove = ImGui::SmallButton("Remove");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this component");
    ImGui::PopID();
    return open;
}

bool ComponentsPanel::draw(Engine::ScenePreset &scene, const Engine::Entity selected, bool& isOpen) {
    ImGui::Begin("Inspector", &isOpen);
    if (selected == Engine::NullEntity) {
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
    if (scene.editor().valid(selected) && scene.editor().has<Engine::NameComponent>(selected)) {
        const auto readScene = scene.editor();
        const auto &name = readScene.get<Engine::NameComponent>(selected).value;
        char editableName[260]{};
        std::snprintf(editableName, sizeof(editableName), "%s", name.c_str());
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::InputTextWithHint("##object-name", "Object name", editableName, sizeof(editableName)) &&
            editableName[0] != '\0') {
            scene.editor().modify<Engine::NameComponent>(selected, [&](auto &value) {
                value.value = editableName;
            });
        }
    }
    ImGui::TextDisabled("Rename the object, then tweak its components below.");
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen) &&
        scene.editor().valid(selected) && scene.editor().has<Engine::Transform>(selected)) {
        TransformFields{scene.edit(selected)}.draw();
    }
    if (scene.editor().valid(selected) &&
        scene.editor().has<Engine::MeshRenderer>(selected) &&
        ImGui::CollapsingHeader("Mesh Renderer", ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto readScene = scene.editor();
        const auto &source = readScene.get<Engine::MeshRenderer>(selected);
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
        changed |= ImGui::SliderFloat("Metallic##mesh-material", &renderer.material.metallic,
                                      0.0F, 1.0F, "%.2f");
        changed |= ImGui::SliderFloat("Roughness##mesh-material", &renderer.material.roughness,
                                      0.0F, 1.0F, "%.2f");
        changed |= ImGui::SliderFloat("Ambient Occlusion##mesh-material",
                                      &renderer.material.ambientOcclusion, 0.0F, 1.0F, "%.2f");
        changed |= ImGui::DragFloat("Normal Scale##mesh-material", &renderer.material.normalScale,
                                    0.01F, 0.0F, 10.0F, "%.2f");
        renderer.material.metallic = std::clamp(renderer.material.metallic, 0.0F, 1.0F);
        renderer.material.roughness = std::clamp(renderer.material.roughness, 0.0F, 1.0F);
        renderer.material.ambientOcclusion = std::clamp(renderer.material.ambientOcclusion, 0.0F, 1.0F);
        renderer.material.normalScale = std::max(0.0F, renderer.material.normalScale);

        changed |= ImGui::Checkbox("Alpha Blend##mesh-material", &renderer.material.alphaBlend);
        changed |= ImGui::Checkbox("Double Sided##mesh-material", &renderer.material.doubleSided);
        if (!renderer.material.alphaBlend) {
            changed |= ImGui::SliderFloat("Alpha Cutoff##mesh-material", &renderer.material.alphaCutoff,
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
            scene.editor().modify<Engine::MeshRenderer>(selected,
                [&](auto &component) { component = renderer; });
        }
    }
    if (scene.editor().valid(selected) &&
        scene.editor().has<Engine::TerrainComponent>(selected) &&
        ImGui::CollapsingHeader("Terrain", ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto& terrain = scene.editor().get<Engine::TerrainComponent>(selected);
        ImGui::Text("Heightmap: %u x %u", terrain.resolution, terrain.resolution);
        ImGui::Text("Size: %.1f x %.1f", terrain.width, terrain.depth);
        ImGui::Text("Height range: %.1f to %.1f", terrain.minimumHeight, terrain.maximumHeight);
        ImGui::Spacing();
        ImGui::TextWrapped("Use Sculpt in the Scene View toolbar, then drag the left mouse button over the terrain.");
        if (scene.editor().has<Engine::TerrainGrassComponent>(selected)) {
            const auto& grass = scene.editor().get<Engine::TerrainGrassComponent>(selected);
            ImGui::Text("Grass instances: %zu", grass.instances.size());
            if (!grass.instances.empty() && ImGui::Button("Clear grass")) {
                auto cleared = grass;
                cleared.instances.clear();
                scene.editor().remove<Engine::TerrainGrassComponent>(selected);
                scene.editor().add<Engine::TerrainGrassComponent>(selected, std::move(cleared));
            }
            ImGui::SameLine();
            ImGui::TextDisabled("GPU instanced, shadows off by default");
            if (!grass.instances.empty() && ImGui::Button("Reset trampled grass")) {
                scene.editor().modify<Engine::TerrainGrassComponent>(selected, [](auto& component) {
                    for (auto& instance : component.instances) {
                        instance.bendX = 0.0F;
                        instance.bendZ = 0.0F;
                        instance.trampled = 0.0F;
                    }
                    component.allInstancesDirty = true;
                });
            }
        } else {
            ImGui::TextDisabled("Grass: choose Grass in Scene View and drop a model prefab.");
        }
    }
    if (scene.editor().valid(selected) &&
        scene.editor().has<Engine::LightComponent>(selected)) {
        bool remove = false;
        const bool open = drawRemovableComponentHeader("Light", "light", remove);
        if (remove) {
            scene.editor().remove<Engine::LightComponent>(selected);
        } else if (open) {
            const auto source = scene.editor().get<Engine::LightComponent>(selected);
            auto light = source;
            const bool hasColorPicker = scene.editor().has<Engine::ColorPickerComponent>(selected);
            if (hasColorPicker) {
                light.color = scene.editor().get<Engine::ColorPickerComponent>(selected).color;
            }
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
            changed |= ImGui::Checkbox("Enabled##light", &light.enabled);
            changed |= ImGui::Checkbox("Cast Shadows##light", &light.castShadows);
            light.intensity = std::max(0.0F, light.intensity);

            if (changed) {
                scene.editor().modify<Engine::LightComponent>(selected,
                    [&](auto &component) { component = light; });
                if (hasColorPicker) {
                    scene.editor().modify<Engine::ColorPickerComponent>(selected,
                        [&](auto &component) { component.color = light.color; });
                }
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
                readScene.get<Engine::SmokeEmitterComponent>(selected).emitter;
        auto emitter = source;
        const bool hasColorPicker =
                readScene.has<Engine::ColorPickerComponent>(selected);
        if (hasColorPicker) {
            emitter.color = readScene.get<Engine::ColorPickerComponent>(selected).color;
        }

        bool changed = false;
        bool colorChanged = false;
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
            colorChanged = true;
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
            scene.editor().modify<Engine::SmokeEmitterComponent>(selected,
                                                                 [&](auto &component) {
                                                                     component.emitter = emitter;
                                                                 });
            if (colorChanged && hasColorPicker) {
                scene.editor().modify<Engine::ColorPickerComponent>(selected,
                                                                    [&](auto &component) {
                                                                        component.color = emitter.color;
                                                                    });
            }
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
        const auto &script = readScene.get<Engine::ScriptComponent>(selected);
        char className[260]{};
        std::snprintf(className, sizeof(className), "%s", script.className.c_str());
        ImGui::TextDisabled("C++ script class");
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::InputText("##script-class", className, sizeof(className))) {
            scene.editor().modify<Engine::ScriptComponent>(selected, [&](auto &value) {
                value.className = className;
                value.reset();
            });
        }
        bool enabled = script.enabled;
        if (ImGui::Checkbox("Enabled##script", &enabled)) {
            scene.editor().modify<Engine::ScriptComponent>(selected, [&](auto &value) {
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
            const auto collider = scene.editor().get<Engine::ColliderComponent>(selected);
            int shape = static_cast<int>(collider.shape.index());
            const char *shapeNames[] = {"Box", "Sphere", "Capsule", "Ramp", "Mesh"};
            const bool hasMesh = scene.editor().has<Engine::MeshRenderer>(selected) &&
                scene.editor().get<Engine::MeshRenderer>(selected).hasMesh();
            const bool hasBody = scene.editor().has<Engine::RigidbodyComponent>(selected);
            const Engine::RigidbodyType bodyType = hasBody
                ? scene.editor().get<Engine::RigidbodyComponent>(selected).type
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
                                                        scene.editor().get<Engine::MeshRenderer>(selected).mesh}};
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
            changed |= ImGui::SliderFloat("Restitution##collider", &value.restitution,
                                          0.0F, 1.0F, "%.2f");
            value.friction = std::max(0.0F, value.friction);
            value.restitution = std::clamp(value.restitution, 0.0F, 1.0F);
            if (changed) {
                scene.editor().modify<Engine::ColliderComponent>(selected,
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
            const auto rigidbody = scene.editor().get<Engine::RigidbodyComponent>(selected);
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

            if (dynamic) {
                ImGui::Separator();
                ImGui::TextDisabled("Initial / overridden velocity");
                float linearVelocity[3] = {value.linearVelocity.x(), value.linearVelocity.y(),
                                           value.linearVelocity.z()};
                float angularVelocity[3] = {value.angularVelocity.x(), value.angularVelocity.y(),
                                            value.angularVelocity.z()};
                if (ImGui::DragFloat3("Linear Velocity##rigidbody", linearVelocity, 0.05F,
                                      -1000.0F, 1000.0F)) {
                    value.linearVelocity = {linearVelocity[0], linearVelocity[1], linearVelocity[2]};
                    changed = true;
                }
                if (ImGui::DragFloat3("Angular Velocity##rigidbody", angularVelocity, 1.0F,
                                      -10000.0F, 10000.0F, "%.1f deg/s")) {
                    value.angularVelocity = {angularVelocity[0], angularVelocity[1], angularVelocity[2]};
                    changed = true;
                }
            }
            value.mass = std::max(0.001F, value.mass);
            value.linearDamping = std::max(0.0F, value.linearDamping);
            value.angularDamping = std::max(0.0F, value.angularDamping);
            if (changed) {
                scene.editor().modify<Engine::RigidbodyComponent>(selected,
                    [&](auto &component) { component = value; });
            }
        }
    }
    if (scene.editor().valid(selected) && scene.editor().has<Engine::ColorPickerComponent>(selected)) {
        bool remove = false;
        const bool open = drawRemovableComponentHeader("Color Picker", "color-picker", remove);
        if (remove) {
            scene.editor().remove<Engine::ColorPickerComponent>(selected);
        } else if (open) {
        const auto readScene = scene.editor();
        const auto &picker =
                readScene.get<Engine::ColorPickerComponent>(selected);
        float rgba[4] = {picker.color.r(), picker.color.g(), picker.color.b(), picker.color.a()};
        if (ImGui::ColorEdit4("Color", rgba, ImGuiColorEditFlags_AlphaBar)) {
            const Engine::Color color{rgba[0], rgba[1], rgba[2], rgba[3]};
            scene.editor().modify<Engine::ColorPickerComponent>(selected, [&](auto &component) {
                component.color = color;
            });
            if (scene.editor().has<Engine::LightComponent>(selected)) {
                scene.editor().modify<Engine::LightComponent>(selected, [&](auto &component) {
                    component.color = color;
                });
            }
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
        const bool hasScript = scene.editor().has<Engine::ScriptComponent>(selected);
        const bool hasColorPicker = scene.editor().has<Engine::ColorPickerComponent>(selected);
        const bool hasCollider = scene.editor().has<Engine::ColliderComponent>(selected);
        const bool hasRigidbody = scene.editor().has<Engine::RigidbodyComponent>(selected);
        const bool hasSmokeEmitter = scene.editor().has<Engine::SmokeEmitterComponent>(selected);
        const bool hasLight = scene.editor().has<Engine::LightComponent>(selected);
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
        if (ImGui::MenuItem("Light", nullptr, false, !hasLight)) {
            scene.editor().add<Engine::LightComponent>(selected);
            ImGui::CloseCurrentPopup();
        }
        if (hasLight) ImGui::TextDisabled("Light component already added");
        ImGui::EndPopup();
    }
    if (EditorButton("Attach C++ Script", {-1.0F, 0.0F}).draw()) ImGui::OpenPopup("Attach C++ Script");
    if (ImGui::BeginPopupModal("Attach C++ Script", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static char attachedClassName[128]{};
        ImGui::TextUnformatted("Enter the registered C++ script class name.");
        ImGui::InputTextWithHint("Class name", "CubeMovement", attachedClassName, sizeof(attachedClassName));
        const bool validName = attachedClassName[0] != '\0';
        if (EditorButton("Attach", {100.0F, 0.0F}).draw() && validName) {
            if (!scene.editor().has<Engine::ScriptComponent>(selected)) {
                scene.editor().add<Engine::ScriptComponent>(selected);
            }
            scene.editor().modify<Engine::ScriptComponent>(selected, [&](auto &script) {
                script.className = attachedClassName;
                script.enabled = true;
                script.reset();
            });
            attachedClassName[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (EditorButton("Cancel", {100.0F, 0.0F}).draw()) {
            attachedClassName[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (EditorButton("Create C++ Script", {-1.0F, 0.0F}).draw()) ImGui::OpenPopup("Create C++ Script");
    if (ImGui::BeginPopupModal("Create C++ Script", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static char name[128]{};
        static std::string error;
        ImGui::TextUnformatted("Creates Sandbox/Source/Scripts/<Name>.h and .cpp");
        ImGui::InputTextWithHint("Class name", "PlayerController", name, sizeof(name));
        if (!error.empty()) ImGui::TextColored({1, .3F, .3F, 1}, "%s", error.c_str());
        if (EditorButton("Create").draw() && EditorSceneSession::createCppScript(name, error)) {
            if (!scene.editor().has<Engine::ScriptComponent>(selected))
                scene.editor().add<
                    Engine::ScriptComponent>(selected);
            scene.editor().modify<Engine::ScriptComponent>(selected, [&](auto &script) {
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
