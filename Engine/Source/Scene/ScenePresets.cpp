#include "Engine/Scene/ScenePresets.h"

#include "Engine/Core/Transform.h"
#include "Engine/ECS/Components/CameraComponent.h"
#include "Engine/ECS/Components/ParticleEmitterComponent.h"
#include "Engine/ECS/Components/ColorPickerComponent.h"
#include "Engine/ECS/Components/ColliderComponent.h"
#include "Engine/ECS/Components/RigidbodyComponent.h"
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

// NOLINTBEGIN(readability-magic-numbers)

namespace Engine {
    namespace {
        constexpr std::size_t CubesPerAxis = 30;
        constexpr float CubeSpacing = 1.25F;
        constexpr std::size_t DefaultFontCandidateCount = 5;
        constexpr int DefaultFontPixelHeight = 24;
        constexpr int DefaultFontFirstCharacter = 32;
        constexpr int DefaultFontLastCharacter = 126;

        constexpr float ParticleHeight = 0.25F;
        constexpr float GroundPlanePadding = 4.0F;
        constexpr float GroundColliderHalfHeight = 0.05F;
        constexpr float GroundColliderOffsetY = -0.05F;

        constexpr float CubeHalfHeight = 0.5F;
        constexpr float CubeColor = 0.72F;
        constexpr float CubeMetallic = 0.05F;
        constexpr float CubeRoughness = 0.62F;

        constexpr float CameraFieldOfView = 45.0F;
        constexpr float CameraNearClip = 0.1F;
        constexpr float CameraFarClip = 100000.0F;
        constexpr float CameraViewportWidth = 800.0F;
        constexpr float CameraViewportHeight = 600.0F;
        constexpr float ParticleCameraTargetY = 3.0F;
        constexpr float ParticleCameraX = 6.0F;
        constexpr float ParticleCameraY = 5.8F;
        constexpr float ParticleCameraZ = 8.0F;
        constexpr float CubeCameraXScale = 2.9F;
        constexpr float CubeCameraYScale = 2.6F;
        constexpr float CubeCameraZScale = 3.9F;

        constexpr float EditorPlaneScale = 2.0F;
        constexpr float EditorObjectHeight = 0.5F;
        constexpr float RampObjectHeight = 2.0F;
        constexpr float SphereRadius = 0.5F;
        constexpr int DefaultCullingBatch = 1;

        constexpr float GroundColorRed = 0.24F;
        constexpr float GroundColorGreen = 0.16F;
        constexpr float GroundColorBlue = 0.08F;
        constexpr float GroundRoughness = 0.9F;
        constexpr float SphereColorRed = 0.35F;
        constexpr float SphereColorGreen = 0.65F;
        constexpr float SphereColorBlue = 0.95F;
        constexpr float SphereMetallic = 0.1F;
        constexpr float SphereRoughness = 0.42F;
        constexpr float RampColorRed = 0.95F;
        constexpr float RampColorGreen = 0.62F;
        constexpr float RampColorBlue = 0.25F;
        constexpr float RampMetallic = 0.0F;
        constexpr float RampRoughness = 0.72F;

        void buildFont(UI::UIFontAtlas &atlas) {
            const std::array<std::filesystem::path, DefaultFontCandidateCount> candidates{
                "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
                "C:/Windows/Fonts/consola.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
            };
            const auto font = std::ranges::find_if(candidates, [](const auto &path) {
                return std::filesystem::is_regular_file(path);
            });
            if (font == candidates.end()) {
                throw std::runtime_error("No default TrueType font found");
            }
            if (const std::string error = atlas.build(font->string(), DefaultFontPixelHeight,
                                                      DefaultFontFirstCharacter, DefaultFontLastCharacter);
                !error.empty()) {
                throw std::runtime_error("Could not build sample font atlas: " + error);
            }
        }
    } // namespace

    ScenePreset::ScenePreset(const SceneType type) {
        // These meshes back the editor's GameObject menu.  Initializing them
        // is independent from adding sample objects to the new scene.
        planeMesh_ = std::make_shared<Mesh>(Plane::createMesh());
        cubeMesh_ = std::make_shared<Mesh>(Cube::createMesh());
        sphereMesh_ = std::make_shared<Mesh>(Sphere::createMesh());
        rampMesh_ = std::make_shared<Mesh>(Ramp::createMesh());
        buildFont(uiFontAtlas());
        if (type == SceneType::Particles) {
            Particles::ParticleEmitter emitter;
            emitter.position = {0.0F, ParticleHeight, 0.0F};
            setParticleEmitter(emitter);
            auto &particleObject = Scene::createGameObject("Particle System");
            particleSystem = particleObject.entity();
            setParticleEntity(particleSystem);
            particleObject.setPosition(particleEmitter().position);
            particleObject.add<ParticleEmitterComponent>(ParticleEmitterComponent{emitter});
            particleObject.add<ColorPickerComponent>(ColorPickerComponent{particleEmitter().color});
        }

        constexpr float halfExtent = (((CubesPerAxis - 1) * CubeSpacing) + 1.0F) * 0.5F;
        if (type != SceneType::Empty) {
            auto &planeObject = createMeshObject("Plane", planeMesh_,
                                                 PBRMaterial{});
            planeObject.setScale({
                (halfExtent * 2) + GroundPlanePadding, 1, (halfExtent * 2) + GroundPlanePadding
            });
            planeObject.setCastShadow(false);
            plane = planeObject.entity();
            planeObject.add<ColliderComponent>(ColliderComponent{
                .shape = BoxCollider{.halfExtents = {CubeHalfHeight, GroundColliderHalfHeight, CubeHalfHeight}},
                .offset = {0.0F, GroundColliderOffsetY, 0.0F},
            });
        }

        if (type == SceneType::Cubes) {
            constexpr float halfGrid = (CubesPerAxis - 1) * CubeSpacing * 0.5F;
            for (std::size_t y = 0; y < CubesPerAxis; ++y) { // NOLINT(readability-identifier-length)
                for (std::size_t z = 0; z < CubesPerAxis; ++z) { // NOLINT(readability-identifier-length)
                    for (std::size_t x = 0; x < CubesPerAxis; ++x) { // NOLINT(readability-identifier-length)
                        auto &cube = createMeshObject("Cube " + std::to_string(x) + " " +
                                                      std::to_string(y) + " " + std::to_string(z), cubeMesh_,
                                                      PBRMaterial{
                                                          .baseColor = {CubeColor, CubeColor, CubeColor},
                                                          .metallic = CubeMetallic,
                                                          .roughness = CubeRoughness,
                                                      });
                        cube.setPosition({
                            (static_cast<float>(x) * CubeSpacing) - halfGrid,
                            (static_cast<float>(y) * CubeSpacing) + CubeHalfHeight,
                            (static_cast<float>(z) * CubeSpacing) - halfGrid
                        });
                        cube.setCastShadow(false);
                        cube.setCullingBatch(DefaultCullingBatch);
                    }
                }
            }
        }

        const bool particleScene = isParticleScene();
        float targetY = halfExtent;
        Vec3 position{};
        if (particleScene) {
            targetY = ParticleCameraTargetY;
            position = {ParticleCameraX, ParticleCameraY, ParticleCameraZ};
        } else if (type == SceneType::Cubes) {
            position = {
                static_cast<float>(halfExtent) * CubeCameraXScale,
                targetY + (static_cast<float>(halfExtent) * CubeCameraYScale),
                static_cast<float>(halfExtent) * CubeCameraZScale
            };
        }
        const Vec3 direction = Vec3{0, targetY, 0} - position;
        auto &cameraObject = createCamera("Camera", CameraComponent{
                                              .fieldOfView = CameraFieldOfView, .nearClip = CameraNearClip,
                                              .farClip = CameraFarClip,
                                              .aspectRatio = CameraViewportWidth / CameraViewportHeight,
        });
        cameraObject.setPosition(position);
        if (type != SceneType::Empty) {
            cameraObject.setRotation({
                Degrees{Radians{std::atan2(direction.y(), Vec2{direction.x(), direction.z()}.length())}}.value(),
                Degrees{Radians{std::atan2(direction.z(), direction.x())}}.value(), 0
            });
        }
        camera = cameraObject.entity();

        // Every editor scene starts with an explicit directional light.  This
        // keeps illumination editable and visible in the hierarchy instead of
        // relying on the renderer's fallback light.
        static_cast<void>(createLight());
    }

    Entity ScenePreset::createGameObject() {
        const Entity entity = Scene::createGameObject("GameObject").entity();
        editorGameObjects.push_back(entity);
        return entity;
    }

    Entity ScenePreset::createCube() {
        auto &object = createMeshObject("Cube", cubeMesh_,
                                        PBRMaterial{
                                            .baseColor = {CubeColor, CubeColor, CubeColor}, .metallic = CubeMetallic,
                                            .roughness = CubeRoughness,
                                        });
        object.setPosition({0, EditorObjectHeight, 0});
        const Entity entity = object.entity();
        object.add<ColliderComponent>();
        editorCubes.push_back(entity);
        return entity;
    }

    Entity ScenePreset::createPlane() {
        auto &object = createMeshObject("Plane", planeMesh_);
        object.setScale({EditorPlaneScale, 1, EditorPlaneScale});
        const Entity entity = object.entity();
        editorPlanes.push_back(entity);
        return entity;
    }

    Entity ScenePreset::createSphere() {
        auto &object = createMeshObject("Sphere", sphereMesh_,
                                        PBRMaterial{
                                            .baseColor = {SphereColorRed, SphereColorGreen, SphereColorBlue},
                                            .metallic = SphereMetallic, .roughness = SphereRoughness,
                                        });
        object.setPosition({0, EditorObjectHeight, 0});
        const Entity entity = object.entity();
        object.add<ColliderComponent>(ColliderComponent{.shape = SphereCollider{.radius = SphereRadius}});
        object.add<RigidbodyComponent>();
        editorSpheres.push_back(entity);
        return entity;
    }

    Entity ScenePreset::createRamp() {
        auto &object = createMeshObject("Ramp", rampMesh_,
                                        PBRMaterial{
                                            .baseColor = {RampColorRed, RampColorGreen, RampColorBlue},
                                            .metallic = RampMetallic, .roughness = RampRoughness,
                                        });
        object.setPosition({0, RampObjectHeight, 0});
        const Entity entity = object.entity();
        object.add<ColliderComponent>(ColliderComponent{.shape = RampCollider{.halfExtents = Ramp::halfExtents()}});
        editorRamps.push_back(entity);
        return entity;
    }

    Entity ScenePreset::createLight() {
        LightComponent light;
        light.intensity = 4.0F;
        auto &object = Scene::createLight("Light", light);
        object.setPosition({3.0F, 5.0F, -3.0F});
        object.setRotation({-55.0F, 35.0F, 0.0F});
        const Entity entity = object.entity();
        editorLights.push_back(entity);
        return entity;
    }

    Entity ScenePreset::createTerrain() {
        const Actor actor = Scene::createTerrain("Terrain");
        const Entity entity = findEntity(actor.id());
        editorTerrains.push_back(entity);
        return entity;
    }
} // namespace Engine

// NOLINTEND(readability-magic-numbers)
