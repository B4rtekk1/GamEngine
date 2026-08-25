#pragma once

#include "Engine/ECS/Registry.h"
#include "Engine/Scripting/ScriptRegistry.h"

namespace Engine {

class Scene;

/** Creates and executes native C++ scripts registered in ScriptRegistry. */
class ScriptSystem final {
public:
    explicit ScriptSystem(ScriptRegistry& scripts) noexcept : scripts_(scripts) {}

    void update(Registry& registry, float deltaTime) const;
    void update(Scene& scene, float deltaTime) const;

private:
    ScriptRegistry& scripts_;
};

} // namespace Engine
