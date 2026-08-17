#pragma once

#include "Engine/ECS/GameObject.h"

namespace Engine {

class Plane final : public GameObject {
public:
    /** @brief Creates and spawns a plane-backed game object. */
    explicit Plane(Registry& registry)
        : GameObject(registry) {
        spawn();
    }

    [[nodiscard]] static Mesh createMesh() {
        return {
            .vertices = {
                {{-0.5f, 0.0f, -0.5f}, {0.70f, 0.70f, 0.70f}, {0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
                {{ 0.5f, 0.0f, -0.5f}, {0.70f, 0.70f, 0.70f}, {1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
                {{ 0.5f, 0.0f,  0.5f}, {0.70f, 0.70f, 0.70f}, {1.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},
                {{-0.5f, 0.0f,  0.5f}, {0.70f, 0.70f, 0.70f}, {0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},
            },
            .indices = {0, 1, 2, 2, 3, 0},
        };
    }

protected:
    void OnSpawn() override {
        meshRenderer().mesh = std::make_shared<Mesh>(createMesh());
    }
};

} // namespace Engine
