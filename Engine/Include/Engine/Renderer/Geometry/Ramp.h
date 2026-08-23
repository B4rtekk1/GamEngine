#pragma once

#include "Engine/Renderer/Geometry/Mesh.h"

namespace Engine {

// A wide wedge with a 45-degree slope. The equal rise and run are important:
// the slope goes from (y, z) = (-2, -2) to (2, 2).
class Ramp final {
public:
    static constexpr Vec3 halfExtents() noexcept { return {3.0f, 2.0f, 2.0f}; }

    [[nodiscard]] static Mesh createMesh() {
        return {
            .vertices = {
                {{-3.0f, -2.0f, -2.0f}, {0.92f, 0.52f, 0.18f}, {0, 0}, {0, -1, 0}},
                {{ 3.0f, -2.0f, -2.0f}, {0.92f, 0.52f, 0.18f}, {1, 0}, {0, -1, 0}},
                {{ 3.0f, -2.0f,  2.0f}, {0.92f, 0.52f, 0.18f}, {1, 1}, {0, -1, 0}},
                {{-3.0f, -2.0f,  2.0f}, {0.92f, 0.52f, 0.18f}, {0, 1}, {0, -1, 0}},
                {{-3.0f, -2.0f, -2.0f}, {0.95f, 0.62f, 0.25f}, {0, 0}, {0, 0.707f, -0.707f}},
                {{ 3.0f, -2.0f, -2.0f}, {0.95f, 0.62f, 0.25f}, {1, 0}, {0, 0.707f, -0.707f}},
                {{ 3.0f,  2.0f,  2.0f}, {0.95f, 0.62f, 0.25f}, {1, 1}, {0, 0.707f, -0.707f}},
                {{-3.0f,  2.0f,  2.0f}, {0.95f, 0.62f, 0.25f}, {0, 1}, {0, 0.707f, -0.707f}},
                {{-3.0f, -2.0f, -2.0f}, {0.82f, 0.40f, 0.12f}, {0, 0}, {-1, 0, 0}},
                {{-3.0f, -2.0f,  2.0f}, {0.82f, 0.40f, 0.12f}, {1, 0}, {-1, 0, 0}},
                {{-3.0f,  2.0f,  2.0f}, {0.82f, 0.40f, 0.12f}, {1, 1}, {-1, 0, 0}},
                {{ 3.0f, -2.0f, -2.0f}, {0.82f, 0.40f, 0.12f}, {0, 0}, {1, 0, 0}},
                {{ 3.0f,  2.0f,  2.0f}, {0.82f, 0.40f, 0.12f}, {1, 1}, {1, 0, 0}},
                {{ 3.0f, -2.0f,  2.0f}, {0.82f, 0.40f, 0.12f}, {0, 1}, {1, 0, 0}},
                {{-3.0f, -2.0f, 2.0f}, {0.78f, 0.34f, 0.10f}, {0, 0}, {0, 0, 1}},
                {{ 3.0f, -2.0f, 2.0f}, {0.78f, 0.34f, 0.10f}, {1, 0}, {0, 0, 1}},
                {{ 3.0f,  2.0f, 2.0f}, {0.78f, 0.34f, 0.10f}, {1, 1}, {0, 0, 1}},
                {{-3.0f,  2.0f, 2.0f}, {0.78f, 0.34f, 0.10f}, {0, 1}, {0, 0, 1}},
            },
            // The triangular end caps use both windings. Their projection
            // can otherwise be back-face culled from one side of the ramp.
            .indices = {0, 2, 1, 2, 0, 3, 4, 5, 6, 6, 7, 4,
                        8, 9, 10, 10, 9, 8,
                        11, 13, 12, 12, 13, 11,
                        14, 16, 15, 16, 14, 17},
        };
    }
};

} // namespace Engine
