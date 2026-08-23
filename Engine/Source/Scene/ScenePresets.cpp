#include "Engine/Scene/ScenePresets.h"

#include "Engine/Assets/AssetManager.h"
#include "Engine/Core/Transform.h"
#include "Engine/ECS/Components/CameraComponent.h"
#include "Engine/ECS/Components/ParticleEmitterComponent.h"
#include "Engine/ECS/Components/ColorPickerComponent.h"
#include "Engine/ECS/Components/ColliderComponent.h"
#include "Engine/Renderer/Geometry/Cube.h"
#include "Engine/Renderer/Geometry/Plane.h"
#include "Engine/Renderer/MeshRenderer.h"
#include "Engine/Scene/Components/LightComponent.h"
#include "Engine/Scene/SceneBuilder.h"

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
    buildFont(uiFontAtlas());
    SceneBuilder builder{registry()};

    if (type == SceneType::Particles) {
        Particles::ParticleEmitter emitter;
        emitter.position = {0.0f, 0.25f, 0.0f};
        setParticleEmitter(emitter);
        particleSystem = builder.createEntity("Particle System");
        setParticleEntity(particleSystem);
        registry().add<Transform>(particleSystem,
                                Transform{.position = particleEmitter().position});
        registry().add<ParticleEmitterComponent>(particleSystem,
                                                  ParticleEmitterComponent{emitter});
        registry().add<ColorPickerComponent>(particleSystem,
                                           ColorPickerComponent{particleEmitter().color});
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
    plane = builder.createMeshEntity(planeMesh_, Transform{.scale = treeScene ? Vec3{10, 1, 10} : Vec3{halfExtent * 2 + 4, 1, halfExtent * 2 + 4}},
        treeScene ? PBRMaterial{.baseColor = {0.24f, 0.16f, 0.08f}, .roughness = 0.9f} : PBRMaterial{}, false, 0, "Plane");
    registry().add<ColliderComponent>(plane, ColliderComponent{
        .shape = BoxCollider{.halfExtents = {0.5f, 0.05f, 0.5f}},
        .offset = {0.0f, -0.05f, 0.0f}});

    if (treeScene) {
        tree = builder.createMeshEntity(treeMesh_, Transform{.position = {3, 0, 1}},
            PBRMaterial{.baseColor = {0.20f, 0.48f, 0.08f}, .roughness = 0.82f}, true, 0, "Tree");
        static_cast<void>(builder.createLight(Transform{.rotation = {35, -35, 0}}, LightComponent{.type = LightType::Directional, .intensity = 4.0f}, "Directional Light"));
    } else if (type == SceneType::Cubes) {
        constexpr float halfGrid = (CubesPerAxis - 1) * CubeSpacing * 0.5f;
        for (std::size_t y = 0; y < CubesPerAxis; ++y) for (std::size_t z = 0; z < CubesPerAxis; ++z)
            for (std::size_t x = 0; x < CubesPerAxis; ++x) {
                static_cast<void>(builder.createMeshEntity(cubeMesh_, Transform{.position = {x * CubeSpacing - halfGrid, y * CubeSpacing + 0.5f, z * CubeSpacing - halfGrid}},
                    PBRMaterial{.baseColor = {0.72f, 0.72f, 0.72f}, .metallic = 0.05f, .roughness = 0.62f}, false, 1, "Cube"));
            }
    }

    const float targetY = treeScene ? 4.2f : isParticleScene() ? 3.0f : halfExtent;
    const Vec3 position = treeScene ? Vec3{8, 6.5f, 10} : isParticleScene() ? Vec3{6, 5.8f, 8} : Vec3{halfExtent * 2.9f, targetY + halfExtent * 2.6f, halfExtent * 3.9f};
    const Vec3 direction = Vec3{0, targetY, 0} - position;
    camera = builder.createCamera(Transform{.position = position, .rotation = {
        Degrees{Radians{std::atan2(direction.y(), Vec2{direction.x(), direction.z()}.length())}}.value(),
        Degrees{Radians{std::atan2(direction.z(), direction.x())}}.value(), 0}},
        CameraComponent{.fieldOfView = 45, .nearClip = 0.1f, .farClip = 100000, .aspectRatio = 800.0f / 600.0f}, "Camera");
}

Entity ScenePreset::createGameObject() {
    const Entity entity = SceneBuilder{registry()}.createEntity("GameObject");
    registry().add<Transform>(entity);
    registry().add<MeshRenderer>(entity);
    editorGameObjects.push_back(entity);
    return entity;
}

Entity ScenePreset::createCube() {
    const Entity entity = SceneBuilder{registry()}.createMeshEntity(cubeMesh_, Transform{.position = {0, 0.5f, 0}},
        PBRMaterial{.baseColor = {0.72f, 0.72f, 0.72f}, .metallic = 0.05f, .roughness = 0.62f}, true, 0, "Cube");
    registry().add<ColliderComponent>(entity);
    editorCubes.push_back(entity);
    return entity;
}

Entity ScenePreset::createPlane() {
    const Entity entity = SceneBuilder{registry()}.createMeshEntity(planeMesh_, Transform{.scale = {2, 1, 2}}, {}, true, 0, "Plane");
    editorPlanes.push_back(entity);
    return entity;
}

} // namespace Engine
