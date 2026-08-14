#pragma once

namespace Engine {

class Registry;

// Owns and runs the Vulkan rendering loop.
class Renderer final {
public:
    /** @brief Runs the renderer using every renderable entity in registry. */
    void run(Registry& registry);
};

} // namespace Engine
