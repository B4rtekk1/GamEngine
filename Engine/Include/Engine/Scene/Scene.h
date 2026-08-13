#pragma once

#include "Engine/Renderer/Geometry/Cube.h"
#include "Engine/Renderer/Geometry/Plane.h"
#include "Engine/Renderer/MeshRenderer.h"
#include "Engine/Core/Transform.h"
#include "Engine/ECS/Registry.h"

#include <array>
#include <memory>

namespace Engine {

// Default ECS scene: a 10 by 10 by 10 block of cubes standing on a plane.
class Scene final {
public:
    static constexpr std::size_t CubesPerAxis = 10;
    static constexpr std::size_t CubeCount = CubesPerAxis * CubesPerAxis * CubesPerAxis;

    Registry registry;
    Entity plane{NullEntity};
    std::array<Entity, CubeCount> cubes{};

private:
    // Keep the source geometry alive for every renderer component that shares it.
    std::shared_ptr<const Mesh> planeMesh;
    std::shared_ptr<const Mesh> cubeMesh;

public:

    Scene() {
        planeMesh = std::make_shared<Mesh>(Plane::createMesh());
        cubeMesh = std::make_shared<Mesh>(Cube::createMesh());

        plane = registry.create();
        registry.add<Transform>(plane, Transform{
            .scale = {14.0f, 1.0f, 14.0f},
        });
        registry.add<MeshRenderer>(plane, MeshRenderer{
            .mesh = planeMesh,
        });

        constexpr float spacing = 1.25f;
        constexpr float halfGridWidth = (CubesPerAxis - 1) * spacing * 0.5f;

        for (std::size_t layer = 0; layer < CubesPerAxis; ++layer) {
            for (std::size_t row = 0; row < CubesPerAxis; ++row) {
                for (std::size_t column = 0; column < CubesPerAxis; ++column) {
                    const std::size_t index =
                        (layer * CubesPerAxis + row) * CubesPerAxis + column;
                    const Entity cube = registry.create();
                    cubes[index] = cube;

                    registry.add<Transform>(cube, Transform{
                        .position = {
                            static_cast<float>(column) * spacing - halfGridWidth,
                            static_cast<float>(layer) * spacing + 0.5f,
                            static_cast<float>(row) * spacing - halfGridWidth,
                        },
                    });
                    registry.add<MeshRenderer>(cube, MeshRenderer{
                        .mesh = cubeMesh,
                    });
                }
            }
        }
    }
};

} // namespace Engine
