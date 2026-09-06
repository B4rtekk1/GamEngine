#pragma once

#include "Engine/Scene/ScenePresets.h"

#include <functional>
#include <string_view>
#include <vector>

namespace Editor {
    /**
     * Editor-facing description of one ECS component.
     *
     * This is deliberately type-erased: adding a component to the editor no
     * longer requires teaching each menu about its concrete ECS type.
     */
    struct ComponentDescriptor {
        std::string_view name;
        std::string_view category;
        std::string_view description;
        bool singleton{false};
        bool removable{true};

        std::function<bool(Engine::ScenePreset&, Engine::Entity)> canAdd;
        std::function<void(Engine::ScenePreset&, Engine::Entity)> add;
        std::function<void(Engine::ScenePreset&, Engine::Entity)> remove;
        std::function<void(Engine::ScenePreset&, Engine::Entity)> drawInspector;
    };

    class ComponentRegistry final {
    public:
        static ComponentRegistry& instance();

        void registerComponent(ComponentDescriptor descriptor);
        [[nodiscard]] const std::vector<ComponentDescriptor>& components() const noexcept;

    private:
        std::vector<ComponentDescriptor> components_;
    };

    /// Registers all built-in editor components once. Safe to call repeatedly.
    void registerBuiltinComponents();
} // namespace Editor
