#pragma once

#include "imgui.h"

#include "Engine/ECS/GameObject.h"
#include "Engine/Math/Vec3.h"

#include <algorithm>
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
    static constexpr int ComponentCount = 3;                  ///< Number of components in an Engine::Vec3.
    static constexpr int FirstComponent = 0;                  ///< Index of the first editable component.
    static constexpr int LastComponent = ComponentCount - 1; ///< Index of the last editable component.
    static constexpr int XIndex = 0;                          ///< Index of the X component.
    static constexpr int YIndex = 1;                          ///< Index of the Y component.
    static constexpr int ZIndex = 2;                          ///< Index of the Z component.
    static constexpr float LabelRed = 0.62F;                  ///< Red component of the field label color.
    static constexpr float LabelGreen = 0.75F;                ///< Green component of the field label color.
    static constexpr float LabelBlue = 0.80F;                 ///< Blue component of the field label color.
    static constexpr float FullOpacity = 1.0F;                ///< Fully opaque alpha value.
    static constexpr float FullWidth = -1.0F;                 ///< ImGui value requesting all available width.
    static constexpr float NoDragLimit = 0.0F;                ///< ImGui value disabling a drag boundary.
    static constexpr float NoMouseWheel = 0.0F;               ///< Mouse-wheel delta representing no scrolling.

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
     * @brief Draws an editable three-component vector field.
     *
     * Displays a colored label followed by an ImGui drag widget. When the value
     * changes, the supplied callback is invoked with the updated vector.
     * Individual components may also be adjusted using the mouse wheel.
     *
     * @param label Text displayed next to the field.
     * @param widgetId Unique ImGui identifier used by the drag widget.
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
        ImGui::TextDisabled("  Scroll over a value to adjust");
        ImGui::SetNextItemWidth(FullWidth);
        if (!dragFloat3WithWheel(widgetId, values, speed, format)) {
            return;
        }
        update(Engine::Vec3{values[XIndex], values[YIndex], values[ZIndex]});
    }

    /**
     * @brief Returns the game object associated with this field.
     *
     * @return Reference to the associated game object.
     */
    [[nodiscard]] Engine::GameObject& object() const noexcept { return *object_; }

private:
    /**
     * @brief Draws a three-component drag widget with mouse-wheel support.
     *
     * The component under the mouse cursor is incremented or decremented when
     * the user scrolls over the widget.
     *
     * @param label ImGui label or unique widget identifier.
     * @param values Array containing the three editable component values.
     * @param speed Drag speed and mouse-wheel increment.
     * @param format printf-style format used to display component values.
     *
     * @return true if dragging or mouse-wheel input changed a value;
     *         otherwise false.
     */
    static bool dragFloat3WithWheel(const char* label, float values[ComponentCount], const float speed,
                                    const char* format) {
        bool changed = ImGui::DragFloat3(label, values, speed, NoDragLimit, NoDragLimit, format);
        if (!ImGui::IsItemHovered() || ImGui::GetIO().MouseWheel == NoMouseWheel) {
            return changed;
        }

        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const float fieldWidth = (max.x - min.x) / static_cast<float>(ComponentCount);
        const int field = std::clamp(
            static_cast<int>((ImGui::GetIO().MousePos.x - min.x) / fieldWidth), FirstComponent, LastComponent);
        values[field] += ImGui::GetIO().MouseWheel * speed;
        return true;
    }

    Engine::GameObject* object_; ///< Non-owning pointer to the associated game object.
};
