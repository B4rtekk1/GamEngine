#pragma once

#include "EditableField.h"

#include <utility>

/**
 * @brief Provides ImGui controls for editing an object's transform.
 *
 * Displays three editable vector fields corresponding to the position,
 * rotation and scale of the associated Engine::GameObject. Changes made in
 * the widgets are applied to the object immediately.
 *
 * The associated object is supplied through the inherited EditableField
 * constructor.
 */
class TransformFields final : public EditableField {
public:
    /** @brief Inherits constructors used to bind the editor to a game object. */
    using EditableField::EditableField;

    /**
     * @brief Draws controls for the object's position, rotation and scale.
     *
     * Each control is scoped to the address of the currently edited object so
     * that ImGui does not reuse active-widget state after the inspector starts
     * editing another object.
     *
     * Position and scale are displayed with two decimal places. Rotation is
     * displayed in degrees with one decimal place.
     */
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
