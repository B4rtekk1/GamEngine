#pragma once

#include "Engine/Scene/Scene.h"
#include "Engine/Renderer/Geometry/Mesh.h"

#include <memory>
#include <vector>

namespace Engine {

enum class SceneType { Cubes, Tree, Particles };

// Explicit sample-content layer. Applications can use Scene directly and do
// not inherit benchmark geometry, asset paths, or editor labels.
class ScenePreset final : public Scene {
public:
    explicit ScenePreset(SceneType type = SceneType::Cubes);

    [[nodiscard]] Entity createGameObject();
    [[nodiscard]] Entity createCube();
    [[nodiscard]] Entity createPlane();

    Entity plane{NullEntity};
    Entity camera{NullEntity};
    Entity tree{NullEntity};
    std::vector<Entity> editorGameObjects;
    std::vector<Entity> editorCubes;
    std::vector<Entity> editorPlanes;

private:
    std::shared_ptr<const Mesh> planeMesh_;
    std::shared_ptr<const Mesh> cubeMesh_;
    std::shared_ptr<const Mesh> treeMesh_;
};

} // namespace Engine
