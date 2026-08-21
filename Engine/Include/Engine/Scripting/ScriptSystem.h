#pragma once

#include "Engine/ECS/Registry.h"
#include "Engine/Scripting/ScriptRegistry.h"

namespace Engine {

/** Creates and executes native C++ scripts registered in ScriptRegistry. */
class ScriptSystem final {
public:
    explicit ScriptSystem(ScriptRegistry& scripts) noexcept : scripts_(scripts) {}

    void update(Registry& registry, float deltaTime);

private:
    ScriptRegistry& scripts_;
};

} // namespace Engine
