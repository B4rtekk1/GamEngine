#include "Engine/Scene/ScenePresets.h"

#include "Engine/Assets/AssetManager.h"
#include "Engine/Core/Transform.h"
#include "Engine/ECS/Components/CameraComponent.h"
#include "Engine/ECS/Components/ParticleEmitterComponent.h"
#include "Engine/ECS/Components/ColorPickerComponent.h"
#include "Engine/ECS/Components/ColliderComponent.h"
#include "Engine/Renderer/Geometry/Cube.h"
#include "Engine/Renderer/Geometry/Plane.h"
#include "Engine/Renderer/Geometry/Ramp.h"
#include "Engine/Renderer/Geometry/Sphere.h"
#include "Engine/Renderer/MeshRenderer.h"
#include "Engine/Scene/Components/LightComponent.h"

#include <array>
#include <cmath>
#include <filesystem>
#include <ranges>
#include <stdexcept>

namespace Engine {
namespace {
constexpr std::size_t CubesPerAxis = 30;
constexpr float CubeSpacing = 1.25f;

void buildFont(UI::UIFontAtlas& atlas) {
    const std::array<std::filesystem::path, 5> candidates{
        "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/consola.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf"};
    const auto font = std::ranges::find_if(candidates, [](const auto& path) {
        return std::filesystem::is_regular_file(path);
    });
    if (font == candidates.end()) throw std::runtime_error("No default TrueType font found");
    if (const std::string error = atlas.build(font->string(), 24, 32, 126); !error.empty()) {
        throw std::runtime_error("Could not build sample font atlas: " + error);
    }
}
} // namespace

ScenePreset::ScenePreset(const SceneType type) {
    planeMesh_ = std::make_shared<Mesh>(Plane::createMesh());
    cubeMesh_ = std::make_shared<Mesh>(Cube::createMesh());
    sphereMesh_ = std::make_shared<Mesh>(Sphere::createMesh());
    rampMesh_ = std::make_shared<Mesh>(Ramp::createMesh());
    buildFont(uiFontAtlas());
    if (type == SceneType::Particles) {
        Particles::ParticleEmitter emitter;
        emitter.position = {0.0f, 0.25f, 0.0f};
        setParticleEmitter(emitter);
        auto& particleObject = Scene::createGameObject("Particle System");
        particleSystem = particleObject.entity();
        setParticleEntity(particleSystem);
        particleObject.setPosition(particleEmitter().position);
        particleObject.add<ParticleEmitterComponent>(ParticleEmitterComponent{emitter});
        particleObject.add<ColorPickerComponent>(ColorPickerComponent{particleEmitter().color});
    }

    const bool treeScene = type == SceneType::Tree;
    if (treeScene) {
        Assets::AssetManager assets;
        Assets::register_default_asset_loaders(assets);
        const auto loaded = assets.load<Mesh>("Assets/Models/tree.glb", Assets::AssetType::Mesh);
        if (!loaded) throw std::runtime_error("Could not load Assets/Models/tree.glb");
        treeMesh_ = loaded.shared();
    }

    const float halfExtent = ((CubesPerAxis - 1) * CubeSpacing + 1.0f) * 0.5f;
    auto& planeObject = createMeshObject("Plane", planeMesh_,
        treeScene ? PBRMaterial{.baseColor = {0.24f, 0.16f, 0.08f}, .roughness = 0.9f} : PBRMaterial{});
    planeObject.setScale(treeScene ? Vec3{10, 1, 10} : Vec3{halfExtent * 2 + 4, 1, halfExtent * 2 + 4});
    planeObject.setCastShadow(false);
    plane = planeObject.entity();
    planeObject.add<ColliderComponent>(ColliderComponent{
        .shape = BoxCollider{.halfExtents = {0.5f, 0.05f, 0.5f}},
        .offset = {0.0f, -0.05f, 0.0f}});

    if (treeScene) {
        auto& treeObject = createMeshObject("Tree", treeMesh_,
            PBRMaterial{.baseColor = {0.20f, 0.48f, 0.08f}, .roughness = 0.82f});
        treeObject.setPosition({3, 0, 1});
        tree = treeObject.entity();
        auto& light = createLight("Directional Light",
            LightComponent{.type = LightType::Directional, .intensity = 4.0f});
        light.setRotation({35, -35, 0});
    } else if (type == SceneType::Cubes) {
        constexpr float halfGrid = (CubesPerAxis - 1) * CubeSpacing * 0.5f;
        for (std::size_t y = 0; y < CubesPerAxis; ++y) for (std::size_t z = 0; z < CubesPerAxis; ++z)
            for (std::size_t x = 0; x < CubesPerAxis; ++x) {
                auto& cube = createMeshObject("Cube " + std::to_string(x) + " " +
                    std::to_string(y) + " " + std::to_string(z), cubeMesh_,
                    PBRMaterial{.baseColor = {0.72f, 0.72f, 0.72f}, .metallic = 0.05f, .roughness = 0.62f});
                cube.setPosition({x * CubeSpacing - halfGrid, y * CubeSpacing + 0.5f,
                                  z * CubeSpacing - halfGrid});
                cube.setCastShadow(false);
                cube.setCullingBatch(1);
            }
    }

    const float targetY = treeScene ? 4.2f : isParticleScene() ? 3.0f : halfExtent;
    const Vec3 position = treeScene ? Vec3{8, 6.5f, 10} : isParticleScene() ? Vec3{6, 5.8f, 8} : Vec3{halfExtent * 2.9f, targetY + halfExtent * 2.6f, halfExtent * 3.9f};
    const Vec3 direction = Vec3{0, targetY, 0} - position;
    auto& cameraObject = createCamera("Camera", CameraComponent{
        .fieldOfView = 45, .nearClip = 0.1f, .farClip = 100000,
        .aspectRatio = 800.0f / 600.0f});
    cameraObject.setPosition(position);
    cameraObject.setRotation({
        Degrees{Radians{std::atan2(direction.y(), Vec2{direction.x(), direction.z()}.length())}}.value(),
        Degrees{Radians{std::atan2(direction.z(), direction.x())}}.value(), 0});
    camera = cameraObject.entity();
}

Entity ScenePreset::createGameObject() {
    const Entity entity = Scene::createGameObject("GameObject").entity();
    editorGameObjects.push_back(entity);
    return entity;
}

Entity ScenePreset::createCube() {
    auto& object = createMeshObject("Cube", cubeMesh_,
        PBRMaterial{.baseColor = {0.72f, 0.72f, 0.72f}, .metallic = 0.05f, .roughness = 0.62f});
    object.setPosition({0, 0.5f, 0});
    const Entity entity = object.entity();
    object.add<ColliderComponent>();
    editorCubes.push_back(entity);
    return entity;
}

Entity ScenePreset::createPlane() {
    auto& object = createMeshObject("Plane", planeMesh_);
    object.setScale({2, 1, 2});
    const Entity entity = object.entity();
    editorPlanes.push_back(entity);
    return entity;
}

Entity ScenePreset::createSphere() {
    auto& object = createMeshObject("Sphere", sphereMesh_,
        PBRMaterial{.baseColor = {0.35f, 0.65f, 0.95f}, .metallic = 0.1f, .roughness = 0.42f});
    object.setPosition({0, 0.5f, 0});
    const Entity entity = object.entity();
    object.add<ColliderComponent>(ColliderComponent{.shape = SphereCollider{.radius = 0.5f}});
    editorSpheres.push_back(entity);
    return entity;
}

Entity ScenePreset::createRamp() {
    auto& object = createMeshObject("Ramp", rampMesh_,
        PBRMaterial{.baseColor = {0.95f, 0.62f, 0.25f}, .metallic = 0.0f, .roughness = 0.72f});
    object.setPosition({0, 2.0f, 0});
    const Entity entity = object.entity();
    object.add<ColliderComponent>(ColliderComponent{.shape = RampCollider{.halfExtents = Ramp::halfExtents()}});
    editorRamps.push_back(entity);
    return entity;
}

} // namespace Engine
