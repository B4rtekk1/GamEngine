#pragma once

#include "cube.h"
#include "plane.h"

namespace Engine {

// Default scene: a cube (the scene's GameObject) standing on a plane.
class Scene final {
public:
    Plane plane;
    Cube gameObject;

    Scene() {
        plane.scale = {8.0f, 1.0f, 8.0f};
        gameObject.position.setY(0.5f);
    }
};

} // namespace Engine
