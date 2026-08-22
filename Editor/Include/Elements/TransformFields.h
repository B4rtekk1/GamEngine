#pragma once

#include "EditableField.h"

#include "Engine/Core/Transform.h"

#include <utility>

/** @brief Draws and updates the editable fields of an entity transform. */
class TransformFields final : public EditableField {
public:
    using EditableField::EditableField;

    /** @brief Draws position, rotation and scale fields. */
    void draw() const {
        if (!registry().valid(entity()) || !registry().has<Engine::Transform>(entity())) return;

        const Engine::SceneEditor& readScene = registry();
        const Engine::Transform& transform = readScene.get<Engine::Transform>(entity());
        drawVec3Field("Position (X, Y, Z)", "##position", transform.position, 0.05f, "%.2f",
            [this](const Engine::Vec3& value) { updateTransform([&](auto& t) { t.position = value; }); });
        drawVec3Field("Rotation (X, Y, Z)", "##rotation", transform.rotation, 0.5f, "%.1f",
            [this](const Engine::Vec3& value) { updateTransform([&](auto& t) { t.rotation = value; }); });
        drawVec3Field("Scale (X, Y, Z)", "##scale", transform.scale, 0.01f, "%.2f",
            [this](const Engine::Vec3& value) { updateTransform([&](auto& t) { t.scale = value; }); });
    }

private:
    template<typename Update>
    void updateTransform(Update&& update) const {
        registry().modify<Engine::Transform>(entity(), std::forward<Update>(update));
    }
};
