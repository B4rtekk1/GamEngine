#pragma once

#include "imgui.h"

#include "Engine/ECS/GameObject.h"
#include "Engine/Math/Vec3.h"

#include <algorithm>
#include <functional>

class EditableField {
    static constexpr int ComponentCount = 3;
    static constexpr int FirstComponent = 0;
    static constexpr int LastComponent = ComponentCount - 1;
    static constexpr int XIndex = 0;
    static constexpr int YIndex = 1;
    static constexpr int ZIndex = 2;
    static constexpr float LabelRed = 0.62F;
    static constexpr float LabelGreen = 0.75F;
    static constexpr float LabelBlue = 0.80F;
    static constexpr float FullOpacity = 1.0F;
    static constexpr float FullWidth = -1.0F;
    static constexpr float NoDragLimit = 0.0F;
    static constexpr float NoMouseWheel = 0.0F;

public:
    explicit EditableField(Engine::GameObject& object) noexcept : object_(&object) {}

protected:
    ~EditableField() = default;

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

    [[nodiscard]] Engine::GameObject& object() const noexcept { return *object_; }

private:
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

    Engine::GameObject* object_;
};
