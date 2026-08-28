#pragma once

#include "Engine/Renderer/Geometry/Mesh.h"

namespace Engine {
    //NOLINTBEGIN

class Plane final {
public:
    [[nodiscard]] static Mesh createMesh() {
        return {
            .vertices = {
                {{-0.5F, 0.0F, -0.5F}, {0.70F, 0.70F, 0.70F}, {0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}},
                {{ 0.5F, 0.0F, -0.5F}, {0.70F, 0.70F, 0.70F}, {1.0F, 0.0F}, {0.0F, 1.0F, 0.0F}},
                {{ 0.5F, 0.0F,  0.5F}, {0.70F, 0.70F, 0.70F}, {1.0F, 1.0F}, {0.0F, 1.0F, 0.0F}},
                {{-0.5F, 0.0F,  0.5F}, {0.70F, 0.70F, 0.70F}, {0.0F, 1.0F}, {0.0F, 1.0F, 0.0F}},
            },
            .indices = {0, 1, 2, 2, 3, 0},
        };
    }
};

} // namespace Engine
//NOLINTEND