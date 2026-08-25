#pragma once

#include "EditableField.h"

#include <utility>

/** @brief Draws and updates the editable fields of an entity transform. */
class TransformFields final : public EditableField {
public:
    using EditableField::EditableField;

    /** @brief Draws position, rotation and scale fields. */
    void draw() const {
        // The inspector can switch to a freshly duplicated object while the
        // same panel remains alive. Scope the widget IDs to the object so
        // ImGui cannot reuse the previous object's active/input state.
        ImGui::PushID(static_cast<const void*>(&object()));
        drawVec3Field("Position", "##position", object().position(), 0.05F, "%.2F",
            [this](const Engine::Vec3& value) { object().setPosition(value); });
        drawVec3Field("Rotation", "##rotation", object().rotation(), 0.5F, "%.1F°",
            [this](const Engine::Vec3& value) { object().setRotation(value); });
        drawVec3Field("Scale", "##scale", object().scale(), 0.01F, "%.2F",
            [this](const Engine::Vec3& value) { object().setScale(value); });
        ImGui::PopID();
    }
};