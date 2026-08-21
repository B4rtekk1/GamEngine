#pragma once

#include "Engine/Core/Transform.h"
#include "Engine/ECS/Entity.h"
#include "Engine/ECS/Registry.h"

namespace Engine {

/** Base class for native C++ behaviours attached to entities. */
class Script {
public:
    virtual ~Script() = default;

    [[nodiscard]] Entity entity() const noexcept { return entity_; }
    [[nodiscard]] Registry& registry() const noexcept { return *registry_; }
    [[nodiscard]] Transform& transform() const { return registry().get<Transform>(entity_); }

    /** Called once, immediately before the first update. */
    virtual void onCreate() {}
    /** Called once per frame while the ScriptComponent is enabled. */
    virtual void onUpdate(float deltaTime) { (void)deltaTime; }
    /** Called when the runtime instance is removed. */
    virtual void onDestroy() {}

private:
    friend class ScriptSystem;
    void attach(Registry& registry, Entity entity) noexcept {
        registry_ = &registry;
        entity_ = entity;
    }

    Registry* registry_{};
    Entity entity_{NullEntity};
};

} // namespace Engine
