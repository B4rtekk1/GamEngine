#pragma once

#include "EditableField.h"

#include <utility>

/** @brief Draws and updates the editable fields of an entity transform. */
class TransformFields final : public EditableField {
public:
    using EditableField::EditableField;

    /** @brief Draws position, rotation and scale fields. */
    void draw() const {
        drawVec3Field("Position", "##position", object().position(), 0.05f, "%.2f",
            [this](const Engine::Vec3& value) { object().setPosition(value); });
        drawVec3Field("Rotation", "##rotation", object().rotation(), 0.5f, "%.1f°",
            [this](const Engine::Vec3& value) { object().setRotation(value); });
        drawVec3Field("Scale", "##scale", object().scale(), 0.01f, "%.2f",
            [this](const Engine::Vec3& value) { object().setScale(value); });
    }
};
