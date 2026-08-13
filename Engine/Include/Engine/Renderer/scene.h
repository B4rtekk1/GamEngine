#pragma once

#include "cube.h"
#include "plane.h"
#include "MeshRenderer.h"
#include "Engine/Core/Transform.h"
#include "Engine/ECS/Registry.h"

namespace Engine {

// Default ECS scene: a cube standing on a plane.
class Scene final {
public:
    Registry registry;
    Entity plane{NullEntity};
    Entity cube{NullEntity};

    Scene() {
        plane = registry.create();
        registry.add<Transform>(plane, Transform{
            .scale = {8.0f, 1.0f, 8.0f},
        });
        registry.add<MeshRenderer>(plane, MeshRenderer{
            .mesh = Plane::createMesh(),
        });

        cube = registry.create();
        registry.add<Transform>(cube, Transform{
            .position = {0.0f, 0.5f, 0.0f},
        });
        registry.add<MeshRenderer>(cube, MeshRenderer{
            .mesh = Cube::createMesh(),
        });
    }
};

} // namespace Engine
