#pragma once

namespace Engine {
class Scene;

/**
 * Game-wide logic a plugin adds to every game that enables it: weather,
 * analytics, AI directors and similar systems that are not tied to one
 * entity (per-entity logic belongs in a Script).
 *
 * The callbacks run while the game runs: in the Player from start to
 * shutdown, in the Editor only during Play Mode. onFixedUpdate follows the
 * physics step; onUpdate runs once per frame. Do not rely on its order
 * relative to the game's scripts.
 */
class PluginSystem {
  public:
    virtual ~PluginSystem() = default;

    virtual void onPlayStart(Scene& scene) {
        static_cast<void>(scene);
    }
    virtual void onUpdate(Scene& scene, float deltaTime) {
        static_cast<void>(scene);
        static_cast<void>(deltaTime);
    }
    virtual void onFixedUpdate(Scene& scene, float fixedDeltaTime) {
        static_cast<void>(scene);
        static_cast<void>(fixedDeltaTime);
    }
    virtual void onPlayStop(Scene& scene) {
        static_cast<void>(scene);
    }
};
} // namespace Engine
