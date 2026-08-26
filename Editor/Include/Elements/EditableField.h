#pragma once

#include "imgui.h"

#include "Engine/ECS/GameObject.h"
#include "Engine/Math/Vec3.h"

#include <cstdio>
#include <functional>

/**
 * @brief Base class for editor fields associated with a game object.
 *
 * Provides common functionality for drawing editable three-component vector
 * fields with ImGui. Each component can be changed by dragging or by scrolling
 * the mouse wheel over the corresponding input field.
 *
 * @note The referenced game object is not owned by this class and must remain
 *       valid for the entire lifetime of the EditableField instance.
 */
class EditableField {
    static constexpr int ComponentCount = 3;
    static constexpr int FirstComponent = 0;
    static constexpr int XIndex = 0;
    static constexpr int YIndex = 1;
    static constexpr int ZIndex = 2;
    static constexpr float LabelRed = 0.72F;
    static constexpr float LabelGreen = 0.78F;
    static constexpr float LabelBlue = 0.86F;
    static constexpr float FullOpacity = 1.0F;
    static constexpr float NoDragLimit = 0.0F;
    static constexpr float NoMouseWheel = 0.0F;
    static constexpr float AxisBadgeWidth = 18.0F;
    static constexpr float AxisFieldGap = 4.0F;

public:
    /**
     * @brief Constructs an editable field associated with a game object.
     *
     * @param object Game object edited by this field.
     */
    explicit EditableField(Engine::GameObject& object) noexcept : object_(&object) {}

protected:
    /**
     * @brief Destroys the editable field.
     */
    ~EditableField() = default;

    /**
     * @brief Draws an editable three-component vector field with colored XYZ badges.
     *
     * @param label Text displayed above the field.
     * @param widgetId Unique ImGui identifier used by the drag widgets.
     * @param current Current vector value displayed by the field.
     * @param speed Amount by which dragging or one mouse-wheel step changes a component.
     * @param format printf-style format used to display component values.
     * @param update Callback invoked with the updated vector after a change.
     */
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): label and widget ID are intentionally paired UI inputs.
    static void drawVec3Field(const char* label, const char* widgetId, const Engine::Vec3& current,
                              const float speed, const char* format,
                              const std::function<void(const Engine::Vec3&)>& update) {
        float values[ComponentCount] = {current.x(), current.y(), current.z()};
        ImGui::TextColored({LabelRed, LabelGreen, LabelBlue, FullOpacity}, "%s", label);
        ImGui::SameLine();
        ImGui::TextDisabled("(scroll to nudge)");

        constexpr ImVec4 axisColors[ComponentCount] = {
            {0.92F, 0.38F, 0.38F, 1.0F},
            {0.42F, 0.78F, 0.45F, 1.0F},
            {0.40F, 0.62F, 0.95F, 1.0F},
        };
        constexpr const char* axisLabels[ComponentCount] = {"X", "Y", "Z"};

        const float available = ImGui::GetContentRegionAvail().x;
        const float fieldWidth =
            (available - AxisBadgeWidth * static_cast<float>(ComponentCount) -
             AxisFieldGap * static_cast<float>(ComponentCount - 1) -
             ImGui::GetStyle().ItemInnerSpacing.x * static_cast<float>(ComponentCount)) /
            static_cast<float>(ComponentCount);

        bool changed = false;
        ImGui::PushID(widgetId);
        for (int index = FirstComponent; index < ComponentCount; ++index) {
            if (index > FirstComponent) {
                ImGui::SameLine(0.0F, AxisFieldGap);
            }
            ImGui::PushStyleColor(ImGuiCol_Button, axisColors[index]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, axisColors[index]);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, axisColors[index]);
            ImGui::PushStyleColor(ImGuiCol_Text, {1.0F, 1.0F, 1.0F, 1.0F});
            ImGui::Button(axisLabels[index], {AxisBadgeWidth, 0.0F});
            ImGui::PopStyleColor(4);
            ImGui::SameLine(0.0F, ImGui::GetStyle().ItemInnerSpacing.x);
            ImGui::SetNextItemWidth(fieldWidth);
            char id[16];
            std::snprintf(id, sizeof(id), "##%d", index);
            if (ImGui::DragFloat(id, &values[index], speed, NoDragLimit, NoDragLimit, format)) {
                changed = true;
            }
            if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != NoMouseWheel) {
                values[index] += ImGui::GetIO().MouseWheel * speed;
                changed = true;
            }
        }
        ImGui::PopID();

        if (changed) {
            update(Engine::Vec3{values[XIndex], values[YIndex], values[ZIndex]});
        }
    }

    /**
     * @brief Returns the game object associated with this field.
     *
     * @return Reference to the associated game object.
     */
    [[nodiscard]] Engine::GameObject& object() const noexcept { return *object_; }

private:
    Engine::GameObject* object_; ///< Non-owning pointer to the associated game object.
};
