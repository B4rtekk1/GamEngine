#pragma once

#include "Engine/Renderer/Geometry/Mesh.h"

namespace Engine {
    //NOLINTBEGIN
    // A wide wedge with a 45-degree slope. The equal rise and run are important:
    // the slope goes from (y, z) = (-2, -2) to (2, 2).
    class Ramp final {
    public:
        static constexpr Vec3 halfExtents() noexcept { return {3.0F, 2.0F, 2.0F}; }

        [[nodiscard]] static Mesh createMesh() {
            return {
                .vertices = {
                    {{-3.0F, -2.0F, -2.0F}, {0.92F, 0.52F, 0.18F}, {0, 0}, {0, -1, 0}},
                    {{3.0F, -2.0F, -2.0F}, {0.92F, 0.52F, 0.18F}, {1, 0}, {0, -1, 0}},
                    {{3.0F, -2.0F, 2.0F}, {0.92F, 0.52F, 0.18F}, {1, 1}, {0, -1, 0}},
                    {{-3.0F, -2.0F, 2.0F}, {0.92F, 0.52F, 0.18F}, {0, 1}, {0, -1, 0}},
                    {{-3.0F, -2.0F, -2.0F}, {0.95F, 0.62F, 0.25F}, {0, 0}, {0, 0.707F, -0.707F}},
                    {{3.0F, -2.0F, -2.0F}, {0.95F, 0.62F, 0.25F}, {1, 0}, {0, 0.707F, -0.707F}},
                    {{3.0F, 2.0F, 2.0F}, {0.95F, 0.62F, 0.25F}, {1, 1}, {0, 0.707F, -0.707F}},
                    {{-3.0F, 2.0F, 2.0F}, {0.95F, 0.62F, 0.25F}, {0, 1}, {0, 0.707F, -0.707F}},
                    {{-3.0F, -2.0F, -2.0F}, {0.82F, 0.40F, 0.12F}, {0, 0}, {-1, 0, 0}},
                    {{-3.0F, -2.0F, 2.0F}, {0.82F, 0.40F, 0.12F}, {1, 0}, {-1, 0, 0}},
                    {{-3.0F, 2.0F, 2.0F}, {0.82F, 0.40F, 0.12F}, {1, 1}, {-1, 0, 0}},
                    {{3.0F, -2.0F, -2.0F}, {0.82F, 0.40F, 0.12F}, {0, 0}, {1, 0, 0}},
                    {{3.0F, 2.0F, 2.0F}, {0.82F, 0.40F, 0.12F}, {1, 1}, {1, 0, 0}},
                    {{3.0F, -2.0F, 2.0F}, {0.82F, 0.40F, 0.12F}, {0, 1}, {1, 0, 0}},
                    {{-3.0F, -2.0F, 2.0F}, {0.78F, 0.34F, 0.10F}, {0, 0}, {0, 0, 1}},
                    {{3.0F, -2.0F, 2.0F}, {0.78F, 0.34F, 0.10F}, {1, 0}, {0, 0, 1}},
                    {{3.0F, 2.0F, 2.0F}, {0.78F, 0.34F, 0.10F}, {1, 1}, {0, 0, 1}},
                    {{-3.0F, 2.0F, 2.0F}, {0.78F, 0.34F, 0.10F}, {0, 1}, {0, 0, 1}},
                },
                // The triangular end caps use both windings. Their projection
                // can otherwise be back-face culled from one side of the ramp.
                .indices = {
                    0, 2, 1, 2, 0, 3, 4, 5, 6, 6, 7, 4,
                    8, 9, 10, 10, 9, 8,
                    11, 13, 12, 12, 13, 11,
                    14, 16, 15, 16, 14, 17,
                },
            };
        }
    };
} // namespace Engine
//NOLINTEND
