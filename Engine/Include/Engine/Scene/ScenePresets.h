#pragma once

#include "Engine/Renderer/Geometry/Mesh.h"
#include "Engine/Scene/Scene.h"

#include <memory>
#include <vector>

namespace Engine {
enum class SceneType : uint8_t { Empty, Cubes, Particles };

// Explicit sample-content layer. Applications can use Scene directly and do
// not inherit benchmark geometry, asset paths, or editor labels.
class ScenePreset final : public Scene {
  public:
    explicit ScenePreset(SceneType type = SceneType::Empty);

    [[nodiscard]] Entity createGameObject();

    [[nodiscard]] Entity createCube();

    [[nodiscard]] Entity createPlane();

    [[nodiscard]] Entity createSphere();

    [[nodiscard]] Entity createCapsule();

    [[nodiscard]] Entity createRamp();

    [[nodiscard]] Entity createCylinder();

    [[nodiscard]] Entity createCone();

    /** Creates a clear 2 x 1.5 m window pane rendered by GlassPass. */
    [[nodiscard]] Entity createGlassPane();
    [[nodiscard]] Entity createCamera();

    [[nodiscard]] Entity createLight();
    [[nodiscard]] Entity createPointLight();
    [[nodiscard]] Entity createSpotLight();

    [[nodiscard]] Entity createTerrain();

    /** Creates an editable cloud assembled from deterministic 3D puffs. */
    [[nodiscard]] Entity createProceduralCloud();

    [[nodiscard]] Entity createOcean();
    [[nodiscard]] Entity createLake();
    [[nodiscard]] Entity createRiver();

    Entity plane{NullEntity};
    Entity camera{NullEntity};
    Entity particleSystem{NullEntity};
    std::vector<Entity> editorGameObjects;
    std::vector<Entity> editorCubes;
    std::vector<Entity> editorPlanes;
    std::vector<Entity> editorSpheres;
    std::vector<Entity> editorCapsules;
    std::vector<Entity> editorCylinders;
    std::vector<Entity> editorCones;
    std::vector<Entity> editorRamps;
    std::vector<Entity> editorLights;
    std::vector<Entity> editorTerrains;
    std::vector<Entity> editorClouds;

  private:
    std::shared_ptr<const Mesh> planeMesh_;
    std::shared_ptr<const Mesh> cubeMesh_;
    std::shared_ptr<const Mesh> sphereMesh_;
    std::shared_ptr<const Mesh> capsuleMesh_;
    std::shared_ptr<const Mesh> cylinderMesh_;
    std::shared_ptr<const Mesh> coneMesh_;
    std::shared_ptr<const Mesh> rampMesh_;
};
} // namespace Engine
