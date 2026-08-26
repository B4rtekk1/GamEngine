#pragma once

#include "Engine/Scene/Scene.h"
#include "Engine/Renderer/Geometry/Mesh.h"

#include <memory>
#include <vector>

namespace Engine {

enum class SceneType { Cubes, Particles };

// Explicit sample-content layer. Applications can use Scene directly and do
// not inherit benchmark geometry, asset paths, or editor labels.
class ScenePreset final : public Scene {
public:
    explicit ScenePreset(SceneType type = SceneType::Cubes);

    [[nodiscard]] Entity createGameObject();
    [[nodiscard]] Entity createCube();
    [[nodiscard]] Entity createPlane();
    [[nodiscard]] Entity createSphere();
    [[nodiscard]] Entity createRamp();

    Entity plane{NullEntity};
    Entity camera{NullEntity};
    Entity particleSystem{NullEntity};
    std::vector<Entity> editorGameObjects;
    std::vector<Entity> editorCubes;
    std::vector<Entity> editorPlanes;
    std::vector<Entity> editorSpheres;
    std::vector<Entity> editorRamps;

private:
    std::shared_ptr<const Mesh> planeMesh_;
    std::shared_ptr<const Mesh> cubeMesh_;
    std::shared_ptr<const Mesh> sphereMesh_;
    std::shared_ptr<const Mesh> rampMesh_;
};

} // namespace Engine
