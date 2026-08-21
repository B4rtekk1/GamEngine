#pragma once

#include <string>

namespace Engine {

/**
 * @brief Attaches a gameplay script asset to an entity.
 *
 * The component stores only the script reference and its activation state.
 * Script execution is intentionally owned by a scripting system, allowing the
 * ECS component to remain serializable and independent of a scripting backend.
 */
struct ScriptComponent final {
    std::string scriptPath;
    bool enabled{true};
};

} // namespace Engine
