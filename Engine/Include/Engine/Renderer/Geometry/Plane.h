#pragma once

#include "Engine/Renderer/Geometry/Mesh.h"

namespace Engine {

class Plane final {
public:
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
};

} // namespace Engine
