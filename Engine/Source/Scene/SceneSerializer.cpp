#include "Engine/Scene/SceneSerializer.h"

#include "Engine/Assets/AssetManager.h"
#include "Engine/Core/Transform.h"
#include "Engine/ECS/Components/CameraComponent.h"
#include "Engine/ECS/Components/ColliderComponent.h"
#include "Engine/ECS/Components/RigidbodyComponent.h"
#include "Engine/ECS/Components/ScriptComponent.h"
#include "Engine/ECS/Components/ColorPickerComponent.h"
#include "Engine/ECS/Components/ParticleEmitterComponent.h"
#include "Engine/ECS/Components/SmokeEmitterComponent.h"
#include "Engine/ECS/Registry.h"
#include "Engine/Renderer/MeshRenderer.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/Components/LightComponent.h"
#include "Engine/Scene/Components/IdentityComponents.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <limits>
#include <locale>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Engine {
    namespace {
        constexpr std::size_t MaxMeshes = 1'000'000;
        constexpr std::size_t MaxVertices = 50'000'000;
        constexpr std::size_t MaxIndices = 150'000'000;
        constexpr std::size_t MaxEntities = 10'000'000;
        constexpr std::size_t FloatBufferSize = 32;
        constexpr unsigned ColliderFormatVersion = 5;
        constexpr unsigned RigidbodyFormatVersion = 6;
        constexpr unsigned SmokeEmitterFormatVersion = 7;
        constexpr unsigned AssetMeshFormatVersion = 8;
        constexpr float LegacyParticleMinVelocity = 0.8F;
        constexpr float LegacyParticleMinVelocityY = 5.5F;
        constexpr float LegacyParticleMaxVelocityY = 9.0F;
        constexpr float LegacyParticleMinLifetime = 1.2F;
        constexpr float LegacyParticleMaxLifetime = 3.4F;
        constexpr float LegacyParticleMinSize = 0.06F;
        constexpr float LegacyParticleMaxSize = 0.16F;
        constexpr float LegacyParticleSpawnRate = 900.0F;

        [[noreturn]] void invalidScene(const std::string &message) {
            throw std::runtime_error("Invalid scene: " + message);
        }

        void expect(std::istream &input, const std::string_view expected) {
            std::string token;
            input >> token;
            if (input.fail() || token != expected) {
                invalidScene("expected '" + std::string(expected) + "'");
            }
        }

        template<typename T>
        T read(std::istream &input, const std::string_view description) {
            T value{};
            input >> value;
            if (input.fail()) {
                invalidScene("could not read " + std::string(description));
            }
            return value;
        }

        float readFloat(std::istream &input, const std::string_view description) {
            const auto value = read<float>(input, description);
            if (!std::isfinite(value)) {
                invalidScene(std::string(description) + " must be finite");
            }
            return value;
        }

        std::size_t readCount(std::istream &input, const std::string_view description,
                              const std::size_t maximum) {
            const auto value = read<unsigned long long>(input, description);
            if (value > maximum) {
                invalidScene(std::string(description) + " exceeds the supported limit");
            }
            return static_cast<std::size_t>(value);
        }

        bool readBool(std::istream &input, const std::string_view description) {
            const int value = read<int>(input, description);
            if (value != 0 && value != 1) {
                invalidScene(std::string(description) + " must be 0 or 1");
            }
            return value != 0;
        }

        Vec3 readVec3(std::istream &input, const std::string_view description) {
            return {
                readFloat(input, description),
                readFloat(input, description),
                readFloat(input, description),
            };
        }

        Math::Color readColor(std::istream &input, const std::string_view description) {
            const float red = readFloat(input, description);
            const float green = readFloat(input, description);
            const float blue = readFloat(input, description);
            return Math::Color::from_rgb(red, green, blue);
        }

        Math::Color readColorRgba(std::istream &input, const std::string_view description) {
            return {
                readFloat(input, std::string(description) + " red"),
                readFloat(input, std::string(description) + " green"),
                readFloat(input, std::string(description) + " blue"),
                readFloat(input, std::string(description) + " alpha")
            };
        }

        void writeFloat(std::ostream &output, const float value) {
            char buffer[FloatBufferSize];
            const auto result = std::to_chars(std::begin(buffer), std::end(buffer), value);
            if (result.ec != std::errc{}) {
                throw std::runtime_error("Could not format scene float");
            }
            output.write(buffer, result.ptr - buffer);
        }

        void writeVec3(std::ostream &output, const Vec3 &value) {
            writeFloat(output, value.x());
            output << ' ';
            writeFloat(output, value.y());
            output << ' ';
            writeFloat(output, value.z());
        }

        void writeColor(std::ostream &output, const Math::Color &value) {
            writeFloat(output, value.r());
            output << ' ';
            writeFloat(output, value.g());
            output << ' ';
            writeFloat(output, value.b());
        }

        void writeColorRgba(std::ostream &output, const Math::Color &value) {
            writeFloat(output, value.r());
            output << ' ';
            writeFloat(output, value.g());
            output << ' ';
            writeFloat(output, value.b());
            output << ' ';
            writeFloat(output, value.a());
        }

        void writeMaterial(std::ostream &output, const PBRMaterial &material) {
            writeColorRgba(output, material.baseColor);
            output << ' ';
            writeFloat(output, material.metallic);
            output << ' ';
            writeFloat(output, material.roughness);
            output << ' ';
            writeFloat(output, material.ambientOcclusion);
            output << ' ' << material.baseColorTexture << ' '
                    << material.metallicRoughnessTexture << ' ' << material.normalTexture << ' ';
            writeFloat(output, material.normalScale);
            output << ' ' << static_cast<int>(material.alphaBlend) << ' '
                    << static_cast<int>(material.doubleSided) << ' ';
            writeFloat(output, material.alphaCutoff);
        }

        PBRMaterial readMaterial(std::istream &input) {
            PBRMaterial material;
            material.baseColor = readColorRgba(input, "material base color");
            material.metallic = readFloat(input, "material metallic");
            material.roughness = readFloat(input, "material roughness");
            material.ambientOcclusion = readFloat(input, "material ambient occlusion");
            material.baseColorTexture = read<std::int32_t>(input, "base-color texture index");
            material.metallicRoughnessTexture = read<std::int32_t>(input, "metallic-roughness texture index");
            material.normalTexture = read<std::int32_t>(input, "normal texture index");
            material.normalScale = readFloat(input, "normal scale");
            material.alphaBlend = readBool(input, "alpha-blend flag");
            material.doubleSided = readBool(input, "double-sided flag");
            material.alphaCutoff = readFloat(input, "alpha cutoff");
            return material;
        }

        void writeParticleEmitter(std::ostream &output, const Particles::ParticleEmitter &emitter) {
            writeVec3(output, emitter.position);
            output << ' ';
            writeVec3(output, emitter.minVelocity);
            output << ' ';
            writeVec3(output, emitter.maxVelocity);
            output << ' ';
            writeColorRgba(output, emitter.color);
            output << ' ';
            writeFloat(output, emitter.minLifeTime);
            output << ' ';
            writeFloat(output, emitter.maxLifeTime);
            output << ' ';
            writeFloat(output, emitter.minSize);
            output << ' ';
            writeFloat(output, emitter.maxSize);
            output << ' ';
            writeFloat(output, emitter.spawnRate);
        }

        void writeSmokeEmitter(std::ostream &output, const Particles::SmokeEmitter &emitter) {
            writeParticleEmitter(output, emitter);
            output << ' ';
            writeFloat(output, emitter.buoyancy);
            output << ' ';
            writeFloat(output, emitter.drag);
            output << ' ';
            writeFloat(output, emitter.turbulence);
            output << ' ';
            writeFloat(output, emitter.collisionRadius);
        }

        void writeCollider(std::ostream &output, const ColliderComponent &collider) {
            output << static_cast<int>(collider.shape.index()) << ' ';
            writeVec3(output, collider.offset);
            output << ' ' << static_cast<int>(collider.isTrigger) << ' ';
            writeFloat(output, collider.friction);
            output << ' ';
            writeFloat(output, collider.restitution);
            std::visit([&output]<typename T>(const T &shape) {
                using Shape = std::decay_t<T>;
                if constexpr (std::is_same_v<Shape, BoxCollider> ||
                              std::is_same_v<Shape, RampCollider>) {
                    output << ' ';
                    writeVec3(output, shape.halfExtents);
                } else if constexpr (std::is_same_v<Shape, SphereCollider>) {
                    output << ' ';
                    writeFloat(output, shape.radius);
                } else if constexpr (std::is_same_v<Shape, CapsuleCollider>) {
                    output << ' ';
                    writeFloat(output, shape.radius);
                    output << ' ';
                    writeFloat(output, shape.height);
                }
            }, collider.shape);
            output << '\n';
        }

        void writeRigidbody(std::ostream &output, const RigidbodyComponent &body) {
            output << static_cast<int>(body.type) << ' ';
            writeFloat(output, body.mass);
            output << ' ';
            writeFloat(output, body.linearDamping);
            output << ' ';
            writeFloat(output, body.angularDamping);
            output << ' ';
            output << static_cast<int>(body.useGravity) << ' '
                    << static_cast<int>(body.fixedRotation) << ' ';
            writeVec3(output, body.linearVelocity);
            output << ' ';
            writeVec3(output, body.angularVelocity);
            output << '\n';
        }

        RigidbodyComponent readRigidbody(std::istream &input) {
            RigidbodyComponent body;
            const int type = read<int>(input, "rigidbody type");
            if (type < static_cast<int>(RigidbodyType::Static) ||
                type > static_cast<int>(RigidbodyType::Kinematic)) {
                invalidScene("unknown rigidbody type");
            }
            body.type = static_cast<RigidbodyType>(type);
            body.mass = readFloat(input, "rigidbody mass");
            body.linearDamping = readFloat(input, "rigidbody linear damping");
            body.angularDamping = readFloat(input, "rigidbody angular damping");
            body.useGravity = readBool(input, "rigidbody gravity flag");
            body.fixedRotation = readBool(input, "rigidbody fixed-rotation flag");
            body.linearVelocity = readVec3(input, "rigidbody linear velocity");
            body.angularVelocity = readVec3(input, "rigidbody angular velocity");
            if (body.mass <= 0.0F || body.linearDamping < 0.0F || body.angularDamping < 0.0F) {
                invalidScene("rigidbody values are invalid");
            }
            return body;
        }

        ColliderComponent readCollider(std::istream &input, const unsigned version) {
            const int type = read<int>(input, "collider shape");
            if (type < 0 || type > 4 || (type == 4 && version < SceneSerializer::FormatVersion)) {
                invalidScene("unknown collider shape");
            }
            ColliderComponent collider;
            collider.offset = readVec3(input, "collider offset");
            collider.isTrigger = readBool(input, "collider trigger flag");
            collider.friction = readFloat(input, "collider friction");
            collider.restitution = readFloat(input, "collider restitution");
            if (collider.friction < 0.0F || collider.restitution < 0.0F || collider.restitution > 1.0F) {
                invalidScene("collider material values are invalid");
            }
            if (type == 0) {
                collider.shape = BoxCollider{readVec3(input, "box collider half extents")};
                if (std::get<BoxCollider>(collider.shape).halfExtents.x() <= 0.0F ||
                    std::get<BoxCollider>(collider.shape).halfExtents.y() <= 0.0F ||
                    std::get<BoxCollider>(collider.shape).halfExtents.z() <= 0.0F) {
                    invalidScene("box collider half extents must be positive");
                }
            } else if (type == 1) {
                collider.shape = SphereCollider{readFloat(input, "sphere collider radius")};
                if (std::get<SphereCollider>(collider.shape).radius <= 0.0F) {
                    invalidScene("sphere collider radius must be positive");
                }
            } else if (type == 2) {
                collider.shape = CapsuleCollider{
                    readFloat(input, "capsule collider radius"),
                    readFloat(input, "capsule collider height"),
                };
                const auto &capsule = std::get<CapsuleCollider>(collider.shape);
                if (capsule.radius <= 0.0F || capsule.height <= 0.0F) {
                    invalidScene(
                        "capsule collider dimensions must be positive");
                }
            } else if (type == 3) {
                collider.shape = RampCollider{readVec3(input, "ramp collider half extents")};
                const auto &ramp = std::get<RampCollider>(collider.shape);
                if (ramp.halfExtents.x() <= 0.0F || ramp.halfExtents.y() <= 0.0F || ramp.halfExtents.z() <= 0.0F) {
                    invalidScene("ramp collider half extents must be positive");
                }
            } else {
                // The renderer mesh is assigned after all entity components
                // have been decoded, so its shared geometry is not duplicated
                // in the collider record.
                collider.shape = MeshCollider{};
            }
            return collider;
        }

        Particles::ParticleEmitter readParticleEmitter(std::istream &input) {
            Particles::ParticleEmitter emitter;
            emitter.position = readVec3(input, "particle emitter position");
            emitter.minVelocity = readVec3(input, "particle emitter minimum velocity");
            emitter.maxVelocity = readVec3(input, "particle emitter maximum velocity");
            emitter.color = readColorRgba(input, "particle emitter color");
            emitter.minLifeTime = readFloat(input, "particle emitter minimum lifetime");
            emitter.maxLifeTime = readFloat(input, "particle emitter maximum lifetime");
            emitter.minSize = readFloat(input, "particle emitter minimum size");
            emitter.maxSize = readFloat(input, "particle emitter maximum size");
            emitter.spawnRate = readFloat(input, "particle emitter spawn rate");
            emitter.accumulator = 0.0F;
            if (emitter.minLifeTime < 0.0F || emitter.maxLifeTime < emitter.minLifeTime ||
                emitter.minSize < 0.0F || emitter.maxSize < emitter.minSize ||
                emitter.spawnRate < 0.0F) {
                invalidScene("particle emitter settings are invalid");
            }
            return emitter;
        }

        Particles::SmokeEmitter readSmokeEmitter(std::istream &input) {
            Particles::SmokeEmitter emitter;
            static_cast<Particles::ParticleEmitter &>(emitter) = readParticleEmitter(input);
            emitter.buoyancy = readFloat(input, "smoke buoyancy");
            emitter.drag = readFloat(input, "smoke drag");
            emitter.turbulence = readFloat(input, "smoke turbulence");
            emitter.collisionRadius = readFloat(input, "smoke collision radius");
            if (emitter.buoyancy < 0.0F || emitter.drag < 0.0F || emitter.turbulence < 0.0F ||
                emitter.collisionRadius < 0.0F) {
                invalidScene("smoke emitter settings are invalid");
            }
            return emitter;
        }

        std::vector<Entity> sortedEntities(const Registry &registry) {
            std::vector<Entity> entities;
            entities.reserve(registry.size());
            registry.view<>([&entities](const Entity entity) {
                entities.push_back(entity);
            });
            std::ranges::sort(entities);
            return entities;
        }
    } // namespace

    void SceneSerializer::save(const Scene &scene, const std::filesystem::path &path) {
        save(scene.registry(), path);
    }

    void SceneSerializer::save(const Scene &scene, const std::filesystem::path &path,
                               const std::uint32_t msaaSamples) {
        save(scene.registry(), path, msaaSamples);
    }

    void SceneSerializer::save(const Scene &scene, std::ostream &output) {
        save(scene.registry(), output);
    }

    void SceneSerializer::save(const Scene &scene, std::ostream &output,
                               const std::uint32_t msaaSamples) {
        save(scene.registry(), output, msaaSamples);
    }

    void SceneSerializer::load(Scene &scene, const std::filesystem::path &path) {
        scene.detachObjectHandles();
        try {
            load(scene.registry(), path);
        } catch (...) {
            // Registry loading is transactional. Reconnect the still-valid
            // previous registry to its GameObject wrappers before propagating
            // the actual load error to the editor.
            scene.rebuildObjectHandles();
            throw;
        }
        scene.rebuildObjectHandles();
    }

    void SceneSerializer::load(Scene &scene, const std::filesystem::path &path,
                               std::optional<std::uint32_t> &msaaSamples) {
        scene.detachObjectHandles();
        try {
            load(scene.registry(), path, msaaSamples);
        } catch (...) {
            scene.rebuildObjectHandles();
            throw;
        }
        scene.rebuildObjectHandles();
    }

    void SceneSerializer::load(Scene &scene, std::istream &input) {
        scene.detachObjectHandles();
        try {
            load(scene.registry(), input);
        } catch (...) {
            scene.rebuildObjectHandles();
            throw;
        }
        scene.rebuildObjectHandles();
    }

    void SceneSerializer::load(Scene &scene, std::istream &input,
                               std::optional<std::uint32_t> &msaaSamples) {
        scene.detachObjectHandles();
        try {
            load(scene.registry(), input, msaaSamples);
        } catch (...) {
            scene.rebuildObjectHandles();
            throw;
        }
        scene.rebuildObjectHandles();
    }

    void SceneSerializer::save(const Registry &registry,
                               const std::filesystem::path &path) {
        save(registry, path, 0);
    }

    void SceneSerializer::save(const Registry &registry,
                               const std::filesystem::path &path,
                               const std::uint32_t msaaSamples) {
        std::ofstream output(path);
        if (!output) {
            throw std::runtime_error("Could not open scene for writing: " + path.string());
        }
        save(registry, output, msaaSamples);
        if (!output) {
            throw std::runtime_error("Could not finish writing scene: " + path.string());
        }
    }

    void SceneSerializer::save(const Registry &registry, std::ostream &output) {
        save(registry, output, 0);
    }

    // The serializer's format branches are kept together to make the output
    // grammar explicit and preserve field ordering.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    void SceneSerializer::save(const Registry &registry, std::ostream &output,
                               const std::uint32_t msaaSamples) {
        std::ostringstream serialized;
        serialized.imbue(std::locale::classic());
        const std::vector<Entity> entities = sortedEntities(registry);
        std::vector<std::shared_ptr<const Mesh> > meshes;
        std::unordered_map<const Mesh *, std::size_t> meshIds;

        for (const Entity entity: entities) {
            if (!registry.has<MeshRenderer>(entity)) {
                continue;
            }
            const auto &renderer = registry.get<MeshRenderer>(entity);
            if (renderer.mesh && !meshIds.contains(renderer.mesh.get())) {
                meshIds.emplace(renderer.mesh.get(), meshes.size());
                meshes.push_back(renderer.mesh);
            }
        }

        if (msaaSamples != 0 && msaaSamples != 2 && msaaSamples != 4) {
            throw std::invalid_argument("MSAA samples must be 0, 2 or 4");
        }
        serialized << "GAMENGINE_SCENE " << FormatVersion << '\n';
        serialized << "SETTINGS MSAA " << msaaSamples << '\n';
        serialized << "MESHES " << meshes.size() << '\n';
        for (std::size_t meshId = 0; meshId < meshes.size(); ++meshId) {
            const Mesh &mesh = *meshes[meshId];
            if (!mesh.sourcePath.empty()) {
                serialized << "MESH_ASSET " << meshId << ' '
                           << std::quoted(mesh.sourcePath.lexically_normal().generic_string()) << '\n';
                continue;
            }
            serialized << "MESH " << meshId << ' ' << mesh.vertices.size() << ' '
                    << mesh.indices.size() << ' ' << mesh.materials.size() << ' '
                    << mesh.images.size() << '\n';
            for (const Vertex &vertex: mesh.vertices) {
                serialized << "VERTEX ";
                writeVec3(serialized, vertex.position);
                serialized << ' ';
                writeVec3(serialized, vertex.color);
                serialized << ' ';
                writeFloat(serialized, vertex.texCoord.x());
                serialized << ' ';
                writeFloat(serialized, vertex.texCoord.y());
                serialized << ' ';
                writeVec3(serialized, vertex.normal);
                serialized << ' ';
                writeFloat(serialized, vertex.tangent.x());
                serialized << ' ';
                writeFloat(serialized, vertex.tangent.y());
                serialized << ' ';
                writeFloat(serialized, vertex.tangent.z());
                serialized << ' ';
                writeFloat(serialized, vertex.tangent.w());
                serialized << ' ' << vertex.materialIndex;
                serialized << '\n';
            }
            serialized << "INDICES";
            for (const std::uint32_t index: mesh.indices) {
                serialized << ' ' << index;
            }
            serialized << '\n';
            for (const PBRMaterial &material: mesh.materials) {
                serialized << "MATERIAL ";
                writeMaterial(serialized, material);
                serialized << '\n';
            }
            for (const auto &[width, height, rgbaPixels]: mesh.images) {
                serialized << "IMAGE " << width << ' ' << height << ' '
                        << rgbaPixels.size() << '\n' << "PIXELS";
                for (const std::uint8_t pixel: rgbaPixels) {
                    serialized << ' ' << static_cast<unsigned>(pixel);
                }
                serialized << '\n';
            }
        }

        serialized << "ENTITIES " << entities.size() << '\n';
        for (const Entity entity: entities) {
            serialized << "ENTITY\n";
            const UUID uuid = registry.has<UUIDComponent>(entity)
                                  ? registry.get<UUIDComponent>(entity).value
                                  : entity;
            const std::string name = registry.has<NameComponent>(entity)
                                         ? registry.get<NameComponent>(entity).value
                                         : "Entity " + std::to_string(entityIndex(entity));
            serialized << "IDENTITY " << uuid << ' ' << std::quoted(name) << '\n';
            if (registry.has<ParentComponent>(entity)) {
                serialized << "PARENT " << registry.get<ParentComponent>(entity).parent << '\n';
            }
            if (registry.has<Transform>(entity)) {
                const auto &transform = registry.get<Transform>(entity);
                serialized << "TRANSFORM ";
                writeVec3(serialized, transform.position);
                serialized << ' ';
                writeVec3(serialized, transform.rotation);
                serialized << ' ';
                writeVec3(serialized, transform.scale);
                serialized << '\n';
            }
            if (registry.has<ColliderComponent>(entity)) {
                serialized << "COLLIDER ";
                writeCollider(serialized, registry.get<ColliderComponent>(entity));
            }
            if (registry.has<RigidbodyComponent>(entity)) {
                serialized << "RIGIDBODY ";
                writeRigidbody(serialized, registry.get<RigidbodyComponent>(entity));
            }
            if (registry.has<MeshRenderer>(entity)) {
                const auto &renderer = registry.get<MeshRenderer>(entity);
                const long long meshId = renderer.mesh
                                             ? static_cast<long long>(meshIds.at(renderer.mesh.get()))
                                             : -1;
                serialized << "MESH_RENDERER " << meshId << ' ';
                writeMaterial(serialized, renderer.material);
                serialized << ' ' << static_cast<int>(renderer.castShadow) << ' '
                        << renderer.cullingBatch << '\n';
            }
            if (registry.has<LightComponent>(entity)) {
                const auto &light = registry.get<LightComponent>(entity);
                serialized << "LIGHT " << static_cast<int>(light.type) << ' ';
                writeColor(serialized, light.color);
                serialized << ' ';
                writeFloat(serialized, light.intensity);
                serialized << ' ' << static_cast<int>(light.enabled) << ' '
                        << static_cast<int>(light.castShadows) << '\n';
            }
            if (registry.has<CameraComponent>(entity)) {
                const auto &camera = registry.get<CameraComponent>(entity);
                serialized << "CAMERA " << static_cast<int>(camera.projection) << ' ';
                writeFloat(serialized, camera.fieldOfView);
                serialized << ' ';
                writeFloat(serialized, camera.orthographicSize);
                serialized << ' ';
                writeFloat(serialized, camera.nearClip);
                serialized << ' ';
                writeFloat(serialized, camera.farClip);
                serialized << ' ';
                writeFloat(serialized, camera.aspectRatio);
                serialized << ' ' << static_cast<int>(camera.primary) << '\n';
            }
            if (registry.has<ColorPickerComponent>(entity)) {
                serialized << "COLOR_PICKER ";
                writeColorRgba(serialized, registry.get<ColorPickerComponent>(entity).color);
                serialized << '\n';
            }
            if (registry.has<ParticleEmitterComponent>(entity)) {
                serialized << "PARTICLE_EMITTER ";
                writeParticleEmitter(serialized,
                                     registry.get<ParticleEmitterComponent>(entity).emitter);
                serialized << '\n';
            }
            if (registry.has<SmokeEmitterComponent>(entity)) {
                serialized << "SMOKE_EMITTER ";
                writeSmokeEmitter(serialized,
                                  registry.get<SmokeEmitterComponent>(entity).emitter);
                serialized << '\n';
            }
            if (registry.has<ScriptComponent>(entity)) {
                const auto &script = registry.get<ScriptComponent>(entity);
                serialized << "SCRIPT " << std::quoted(script.className) << ' '
                        << static_cast<int>(script.enabled) << '\n';
            }
            serialized << "END_ENTITY\n";
        }
        serialized << "END_SCENE\n";

        output << serialized.str();
        if (!output) {
            throw std::runtime_error("Could not write serialized scene");
        }
    }

    void SceneSerializer::load(Registry &registry,
                               const std::filesystem::path &path) {
        std::optional<std::uint32_t> ignoredMsaa;
        load(registry, path, ignoredMsaa);
    }

    void SceneSerializer::load(Registry &registry,
                               const std::filesystem::path &path,
                               std::optional<std::uint32_t> &msaaSamples) {
        std::ifstream input(path);
        if (!input) {
            throw std::runtime_error("Could not open scene for reading: " + path.string());
        }
        load(registry, input, msaaSamples);
    }

    void SceneSerializer::load(Registry &registry, std::istream &input) {
        std::optional<std::uint32_t> ignoredMsaa;
        load(registry, input, ignoredMsaa);
    }

    // The scene format is intentionally decoded in one transaction so that a
    // partially loaded registry is never exposed to the caller. The branches
    // below are format validation and component dispatch, rather than
    // independent business logic that can be simplified without obscuring
    // the on-disk format.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    void SceneSerializer::load(Registry &registry, std::istream &input,
                               std::optional<std::uint32_t> &msaaSamples) {
        msaaSamples.reset();
        input.imbue(std::locale::classic());
        expect(input, "GAMENGINE_SCENE");
        const auto version = read<unsigned>(input, "format version");
        if (version != 3 && version != 4 && version != ColliderFormatVersion &&
            version != RigidbodyFormatVersion &&
            version != 8 &&
            version != FormatVersion) {
            invalidScene("unsupported format version " + std::to_string(version));
        }

        std::string section;
        input >> section;
        if (!input) {
            invalidScene("expected 'MESHES'");
        }
        if (section == "SETTINGS") {
            expect(input, "MSAA");
            const auto samples = read<unsigned>(input, "MSAA sample count");
            if (samples != 0 && samples != 2 && samples != 4) {
                invalidScene("MSAA sample count must be 0, 2 or 4");
            }
            msaaSamples = samples;
            expect(input, "MESHES");
        } else if (section != "MESHES") {
            invalidScene("expected 'MESHES'");
        }
        const std::size_t meshCount = readCount(input, "mesh count", MaxMeshes);
        std::vector<std::shared_ptr<const Mesh> > meshes;
        meshes.reserve(meshCount);
        Assets::AssetManager assetManager;
        for (std::size_t meshIndex = 0; meshIndex < meshCount; ++meshIndex) {
            const auto recordType = read<std::string>(input, "mesh record type");
            if (recordType != "MESH" && recordType != "MESH_ASSET") {
                invalidScene("expected 'MESH' or 'MESH_ASSET'");
            }
            const std::size_t serializedId = readCount(input, "mesh id", MaxMeshes);
            if (serializedId != meshIndex) {
                invalidScene("mesh identifiers must be contiguous");
            }
            if (recordType == "MESH_ASSET") {
                if (version < AssetMeshFormatVersion) {
                    invalidScene("asset mesh references require scene format version 8");
                }
                std::string sourcePath;
                input >> std::quoted(sourcePath);
                if (!input || sourcePath.empty()) {
                    invalidScene("could not read mesh asset path");
                }
                auto mesh = assetManager.loadMesh(std::filesystem::path{sourcePath}).shared();
                if (!mesh) {
                    invalidScene("could not load mesh asset '" + sourcePath + "'");
                }
                meshes.push_back(std::move(mesh));
                continue;
            }
            const std::size_t vertexCount = readCount(input, "vertex count", MaxVertices);
            const std::size_t indexCount = readCount(input, "index count", MaxIndices);
            const std::size_t materialCount = readCount(input, "material count", MaxMeshes);
            const std::size_t imageCount = readCount(input, "image count", MaxMeshes);

            auto mesh = std::make_shared<Mesh>();
            mesh->vertices.reserve(vertexCount);
            for (std::size_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex) {
                expect(input, "VERTEX");
                Vertex vertex;
                vertex.position = readVec3(input, "vertex position");
                vertex.color = readVec3(input, "vertex color");
                vertex.texCoord = {
                    readFloat(input, "texture coordinate"),
                    readFloat(input, "texture coordinate"),
                };
                vertex.normal = readVec3(input, "vertex normal");
                // Read sequentially before construction. MSVC may evaluate the
                // conversion used by braced assignment right-to-left, which
                // swapped tangent.x with tangent.w during scene round-trips.
                const float tangentX = readFloat(input, "tangent x");
                const float tangentY = readFloat(input, "tangent y");
                const float tangentZ = readFloat(input, "tangent z");
                const float tangentW = readFloat(input, "tangent handedness");
                vertex.tangent = Vec4{tangentX, tangentY, tangentZ, tangentW};
                vertex.materialIndex = read<std::uint32_t>(input, "vertex material index");
                mesh->vertices.push_back(vertex);
            }
            expect(input, "INDICES");
            mesh->indices.reserve(indexCount);
            for (std::size_t index = 0; index < indexCount; ++index) {
                const auto value = read<unsigned long long>(input, "mesh index");
                if (value > std::numeric_limits<std::uint32_t>::max() ||
                    value >= mesh->vertices.size()) {
                    invalidScene("mesh index is outside the vertex array");
                }
                mesh->indices.push_back(static_cast<std::uint32_t>(value));
            }
            mesh->materials.reserve(materialCount);
            for (std::size_t material = 0; material < materialCount; ++material) {
                expect(input, "MATERIAL");
                mesh->materials.push_back(readMaterial(input));
            }
            mesh->images.reserve(imageCount);
            for (std::size_t image = 0; image < imageCount; ++image) {
                expect(input, "IMAGE");
                Mesh::Image value;
                value.width = read<std::uint32_t>(input, "image width");
                value.height = read<std::uint32_t>(input, "image height");
                const std::size_t pixelCount = readCount(input, "image pixel count", MaxIndices * 4);
                if (value.width == 0 || value.height == 0 || pixelCount != static_cast<std::size_t>(value.width) * value
                    .height * 4) {
                    invalidScene("image dimensions do not match RGBA pixel data");
                }
                expect(input, "PIXELS");
                value.rgbaPixels.reserve(pixelCount);
                for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
                    const auto byte = read<unsigned>(input, "image pixel");
                    if (byte > std::numeric_limits<std::uint8_t>::max()) {
                        invalidScene("image pixel is outside byte range");
                    }
                    value.rgbaPixels.push_back(static_cast<std::uint8_t>(byte));
                }
                mesh->images.push_back(std::move(value));
            }
            meshes.push_back(std::move(mesh));
        }

        expect(input, "ENTITIES");
        const std::size_t entityCount = readCount(input, "entity count", MaxEntities);
        Registry loaded;
        for (std::size_t entityIndex = 0; entityIndex < entityCount; ++entityIndex) {
            expect(input, "ENTITY");
            const Entity entity = loaded.create();
            bool hasTransform = false;
            bool hasRenderer = false;
            bool hasLight = false;
            bool hasCamera = false;
            bool hasScript = false;
            bool hasColorPicker = false;
            bool hasParticleEmitter = false;
            bool hasSmokeEmitter = false;
            bool hasCollider = false;
            bool hasRigidbody = false;
            bool hasIdentity = false;
            bool hasParent = false;

            while (true) {
                const auto component = read<std::string>(input, "component name");
                if (component == "END_ENTITY") {
                    break;
                }
                if (component == "IDENTITY") {
                    if (version < 4 || hasIdentity) {
                        invalidScene("entity contains an invalid IDENTITY component");
                    }
                    UUIDComponent uuid;
                    uuid.value = read<UUID>(input, "object UUID");
                    if (uuid.value == NullUUID) {
                        invalidScene("object UUID cannot be zero");
                    }
                    NameComponent name;
                    input >> std::quoted(name.value);
                    if (!input || name.value.empty()) {
                        invalidScene("could not read a non-empty object name");
                    }
                    loaded.add<UUIDComponent>(entity, uuid);
                    loaded.add<NameComponent>(entity, std::move(name));
                    reserveUUID(uuid.value);
                    hasIdentity = true;
                } else if (component == "PARENT") {
                    if (version < 4 || hasParent) {
                        invalidScene("entity contains an invalid PARENT component");
                    }
                    const UUID parent = read<UUID>(input, "parent UUID");
                    if (parent == NullUUID) {
                        invalidScene("parent UUID cannot be zero");
                    }
                    loaded.add<ParentComponent>(entity, ParentComponent{.parent = parent});
                    hasParent = true;
                } else if (component == "TRANSFORM") {
                    if (hasTransform) {
                        invalidScene("entity contains more than one Transform");
                    }
                    hasTransform = true;
                    loaded.add<Transform>(entity, Transform{
                                              .position = readVec3(input, "transform position"),
                                              .rotation = readVec3(input, "transform rotation"),
                                              .scale = readVec3(input, "transform scale"),
                                          });
                } else if (component == "COLLIDER") {
                    if (version < ColliderFormatVersion || hasCollider) {
                        invalidScene("entity contains an invalid ColliderComponent");
                    }
                    hasCollider = true;
                    loaded.add<ColliderComponent>(entity, readCollider(input, version));
                } else if (component == "RIGIDBODY") {
                    if (version < RigidbodyFormatVersion || hasRigidbody) {
                        invalidScene("entity contains an invalid RigidbodyComponent");
                    }
                    hasRigidbody = true;
                    loaded.add<RigidbodyComponent>(entity, readRigidbody(input));
                } else if (component == "MESH_RENDERER") {
                    if (hasRenderer) {
                        invalidScene("entity contains more than one MeshRenderer");
                    }
                    hasRenderer = true;
                    const auto meshId = read<long long>(input, "mesh reference");
                    if (meshId < -1 || (meshId >= 0 &&
                                        static_cast<std::size_t>(meshId) >= meshes.size())) {
                        invalidScene("MeshRenderer references an unknown mesh");
                    }
                    MeshRenderer renderer;
                    if (meshId >= 0) {
                        renderer.mesh = meshes[static_cast<std::size_t>(meshId)];
                    }
                    renderer.material = readMaterial(input);
                    renderer.castShadow = readBool(input, "cast-shadow flag");
                    renderer.cullingBatch = read<std::uint32_t>(input, "culling batch");
                    loaded.add<MeshRenderer>(entity, std::move(renderer));
                } else if (component == "LIGHT") {
                    if (hasLight) {
                        invalidScene("entity contains more than one LightComponent");
                    }
                    hasLight = true;
                    const int type = read<int>(input, "light type");
                    if (type < static_cast<int>(LightType::Directional) ||
                        type > static_cast<int>(LightType::Spot)) {
                        invalidScene("unknown light type");
                    }
                    LightComponent light;
                    light.type = static_cast<LightType>(type);
                    light.color = readColor(input, "light color");
                    light.intensity = readFloat(input, "light intensity");
                    light.enabled = readBool(input, "light enabled flag");
                    light.castShadows = readBool(input, "light cast-shadows flag");
                    loaded.add<LightComponent>(entity, light);
                } else if (component == "CAMERA") {
                    if (hasCamera) {
                        invalidScene("entity contains more than one CameraComponent");
                    }
                    hasCamera = true;
                    const int projection = read<int>(input, "camera projection");
                    if (projection < static_cast<int>(CameraProjection::Perspective) ||
                        projection > static_cast<int>(CameraProjection::Orthographic)) {
                        invalidScene("unknown camera projection");
                    }
                    CameraComponent camera;
                    camera.projection = static_cast<CameraProjection>(projection);
                    camera.fieldOfView = readFloat(input, "camera field of view");
                    camera.orthographicSize = readFloat(input, "camera orthographic size");
                    camera.nearClip = readFloat(input, "camera near clip");
                    camera.farClip = readFloat(input, "camera far clip");
                    camera.aspectRatio = readFloat(input, "camera aspect ratio");
                    camera.primary = readBool(input, "camera primary flag");
                    if (!camera.isValid()) {
                        invalidScene("camera settings are invalid");
                    }
                    loaded.add<CameraComponent>(entity, camera);
                } else if (component == "SCRIPT") {
                    if (hasScript) {
                        invalidScene("entity contains more than one ScriptComponent");
                    }
                    hasScript = true;
                    ScriptComponent script;
                    input >> std::quoted(script.className);
                    if (!input) {
                        invalidScene("could not read script class name");
                    }
                    script.enabled = readBool(input, "script enabled flag");
                    loaded.add<ScriptComponent>(entity, std::move(script));
                } else if (component == "COLOR_PICKER") {
                    if (hasColorPicker) {
                        invalidScene("entity contains more than one ColorPickerComponent");
                    }
                    hasColorPicker = true;
                    loaded.add<ColorPickerComponent>(entity, ColorPickerComponent{
                                                         .color = readColorRgba(input, "color picker color"),
                                                     });
                } else if (component == "PARTICLE_EMITTER") {
                    if (hasParticleEmitter) {
                        invalidScene("entity contains more than one ParticleEmitterComponent");
                    }
                    hasParticleEmitter = true;
                    loaded.add<ParticleEmitterComponent>(entity,
                                                         ParticleEmitterComponent{
                                                             .emitter = readParticleEmitter(input),
                                                         });
                } else if (component == "SMOKE_EMITTER") {
                    if (version < SmokeEmitterFormatVersion || hasSmokeEmitter || hasParticleEmitter) {
                        invalidScene("entity contains an invalid SmokeEmitterComponent");
                    }
                    hasSmokeEmitter = true;
                    loaded.add<SmokeEmitterComponent>(entity,
                                                      SmokeEmitterComponent{.emitter = readSmokeEmitter(input)});
                } else {
                    invalidScene("unknown component '" + component + "'");
                }
            }
            if (version >= 4 && !hasIdentity) {
                invalidScene("entity is missing IDENTITY");
            }
            if (version == 3) {
                loaded.add<UUIDComponent>(entity, UUIDComponent{.value = createUUID()});
                loaded.add<NameComponent>(entity, NameComponent{
                                              .value = "Entity " + std::to_string(Engine::entityIndex(entity)),
                                          });
            }
            // Scenes written before ParticleEmitterComponent was serialized may
            // still contain the preset's "Particle System" entity. Reconstruct
            // its emitter from the entity's existing transform and color picker.
            // New scenes always take the PARTICLE_EMITTER branch above.
            if (!hasParticleEmitter && !hasSmokeEmitter && loaded.has<NameComponent>(entity) &&
                loaded.get<NameComponent>(entity).value == "Particle System") {
                Particles::ParticleEmitter emitter;
                // These are the original Particle Scene preset values. Older
                // scene files did not serialize the emitter at all, so using the
                // generic ParticleEmitter defaults would noticeably shrink and
                // thin the restored effect.
                emitter.minVelocity = {
                    -LegacyParticleMinVelocity, LegacyParticleMinVelocityY,
                    -LegacyParticleMinVelocity
                };
                emitter.maxVelocity = {
                    LegacyParticleMinVelocity, LegacyParticleMaxVelocityY,
                    LegacyParticleMinVelocity
                };
                emitter.minLifeTime = LegacyParticleMinLifetime;
                emitter.maxLifeTime = LegacyParticleMaxLifetime;
                emitter.minSize = LegacyParticleMinSize;
                emitter.maxSize = LegacyParticleMaxSize;
                emitter.spawnRate = LegacyParticleSpawnRate;
                if (loaded.has<Transform>(entity)) {
                    emitter.position = loaded.get<Transform>(entity).position;
                }
                if (loaded.has<ColorPickerComponent>(entity)) {
                    emitter.color = loaded.get<ColorPickerComponent>(entity).color;
                }
                loaded.add<ParticleEmitterComponent>(entity,
                                                     ParticleEmitterComponent{.emitter = emitter});
            }
        }

        expect(input, "END_SCENE");
        std::string trailing;
        if (input >> trailing) {
            invalidScene("unexpected data after END_SCENE");
        }

        std::unordered_map<UUID, Entity> entitiesByUuid;
        std::unordered_map<UUID, UUID> parentsByUuid;
        loaded.view<UUIDComponent>([&](const Entity entity, const UUIDComponent &uuid) {
            if (!entitiesByUuid.emplace(uuid.value, entity).second) {
                invalidScene("object UUID is duplicated");
            }
            if (loaded.has<ParentComponent>(entity)) {
                const UUID parent = loaded.get<ParentComponent>(entity).parent;
                if (parent == uuid.value) {
                    invalidScene("object cannot be its own parent");
                }
                parentsByUuid.emplace(uuid.value, parent);
            }
        });
        for (const auto &[child, parent]: parentsByUuid) {
            if (!entitiesByUuid.contains(parent)) {
                invalidScene("parent UUID does not refer to an object in this scene");
            }
            std::unordered_set<UUID> visited;
            UUID current = child;
            auto parentIt = parentsByUuid.find(current);
            while (parentIt != parentsByUuid.end()) {
                current = parentIt->second;
                if (!visited.insert(current).second) { invalidScene("parent hierarchy contains a cycle"); }
                parentIt = parentsByUuid.find(current);
            }
        }

        loaded.view<ColliderComponent, MeshRenderer>([](const Entity,
                                                         ColliderComponent& collider,
                                                         const MeshRenderer& renderer) {
            if (auto* meshCollider = std::get_if<MeshCollider>(&collider.shape)) {
                meshCollider->mesh = renderer.mesh;
            }
        });
        registry = std::move(loaded);
    }
} // namespace Engine
