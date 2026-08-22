#pragma once

#include "imgui.h"

#include "Engine/ECS/Registry.h"
#include "Engine/Math/Vec3.h"

#include <algorithm>
#include <functional>

class EditableField {
public:
    EditableField(Engine::Registry& registry, const Engine::Entity entity) noexcept
        : registry_(registry), entity_(entity) {}

protected:
    ~EditableField() = default;

    void drawVec3Field(const char* label, const char* id, const Engine::Vec3& current,
                       const float speed, const char* format,
                       const std::function<void(const Engine::Vec3&)>& update) const {
        float values[3] = {current.x(), current.y(), current.z()};
        ImGui::TextDisabled("%s", label);
        ImGui::SetNextItemWidth(-1.0f);
        if (!dragFloat3WithWheel(id, values, speed, format)) return;
        update(Engine::Vec3{values[0], values[1], values[2]});
    }

    [[nodiscard]] Engine::Registry& registry() const noexcept { return registry_; }
    [[nodiscard]] Engine::Entity entity() const noexcept { return entity_; }

private:
    static bool dragFloat3WithWheel(const char* label, float values[3], const float speed,
                                    const char* format) {
        bool changed = ImGui::DragFloat3(label, values, speed, 0.0f, 0.0f, format);
        if (!ImGui::IsItemHovered() || ImGui::GetIO().MouseWheel == 0.0f) return changed;

        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const float fieldWidth = (max.x - min.x) / 3.0f;
        const int field = std::clamp(
            static_cast<int>((ImGui::GetIO().MousePos.x - min.x) / fieldWidth), 0, 2);
        values[field] += ImGui::GetIO().MouseWheel * speed;
        return true;
    }

    Engine::Registry& registry_;
    Engine::Entity entity_;
};
