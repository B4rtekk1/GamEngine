#pragma once

#include "Engine/Renderer/Geometry/Cube.h"
#include "Engine/Renderer/Geometry/Plane.h"
#include "Engine/Renderer/MeshRenderer.h"
#include "Engine/Core/Transform.h"
#include "Engine/ECS/Registry.h"
#include "Engine/ECS/Components/CameraComponent.h"

#include <array>
#include <cmath>
#include <memory>

namespace Engine {

class Scene final {
public:
    static constexpr std::size_t CubesPerAxis = 30;
    static constexpr std::size_t CubeCount = CubesPerAxis * CubesPerAxis * CubesPerAxis;
    static constexpr float CubeSpacing = 1.25f;
    static constexpr float GridHalfExtent =
        ((CubesPerAxis - 1) * CubeSpacing + 1.0f) * 0.5f;

    Registry registry;
    Entity plane{NullEntity};
    Entity camera{NullEntity};
    std::array<Entity, CubeCount> cubes{};

private:
    std::shared_ptr<const Mesh> planeMesh;
    std::shared_ptr<const Mesh> cubeMesh;

public:

    Scene() {
        planeMesh = std::make_shared<Mesh>(Plane::createMesh());
        cubeMesh = std::make_shared<Mesh>(Cube::createMesh());

        plane = registry.create();
        registry.add<Transform>(plane, Transform{
            .scale = {GridHalfExtent * 2.0f + 4.0f, 1.0f,
                      GridHalfExtent * 2.0f + 4.0f},
        });
        registry.add<MeshRenderer>(plane, MeshRenderer{
            .mesh = planeMesh,
        });

        camera = registry.create();
        constexpr float cameraTargetY = (CubesPerAxis - 1) * CubeSpacing * 0.5f + 0.5f;
        constexpr Vec3 cameraPosition{
            GridHalfExtent * 2.9f,
            cameraTargetY + GridHalfExtent * 2.6f,
            GridHalfExtent * 3.9f,
        };
        constexpr Vec3 cameraDirection = Vec3{0.0f, cameraTargetY, 0.0f} - cameraPosition;
        const float horizontalDistance = Vec2{cameraDirection.x(), cameraDirection.z()}.length();
        registry.add<Transform>(camera, Transform{
            .position = cameraPosition,
            .rotation = {
                Degrees{Radians{std::atan2(cameraDirection.y(), horizontalDistance)}}.value(),
                Degrees{Radians{std::atan2(cameraDirection.z(), cameraDirection.x())}}.value(),
                0.0f,
            },
        });
        registry.add<CameraComponent>(camera, CameraComponent{
            .fieldOfView = 45.0f,
            .nearClip = 0.1f,
            .farClip = 100'000.0f,
            .aspectRatio = 800.0f / 600.0f,
        });

        constexpr float halfGridWidth = (CubesPerAxis - 1) * CubeSpacing * 0.5f;

        for (std::size_t layer = 0; layer < CubesPerAxis; ++layer) {
            for (std::size_t row = 0; row < CubesPerAxis; ++row) {
                for (std::size_t column = 0; column < CubesPerAxis; ++column) {
                    const std::size_t index =
                        (layer * CubesPerAxis + row) * CubesPerAxis + column;
                    const Entity cube = registry.create();
                    cubes[index] = cube;

                    registry.add<Transform>(cube, Transform{
                        .position = {
                            static_cast<float>(column) * CubeSpacing - halfGridWidth,
                            static_cast<float>(layer) * CubeSpacing + 0.5f,
                            static_cast<float>(row) * CubeSpacing - halfGridWidth,
                        },
                    });
                    registry.add<MeshRenderer>(cube, MeshRenderer{
                        .mesh = cubeMesh,
                        .material = {
                            .baseColor = {0.72f, 0.72f, 0.72f},
                            .metallic = 0.05f,
                            .roughness = 0.62f,
                        },
                        .castShadow = false,
                    });
                }
            }
        }
    }
};

} // namespace Engine
