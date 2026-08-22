#include "Engine/Scene/SceneSerializer.h"

#include "Engine/Core/Transform.h"
#include "Engine/ECS/Components/CameraComponent.h"
#include "Engine/ECS/Components/ScriptComponent.h"
#include "Engine/ECS/Components/ColorPickerComponent.h"
#include "Engine/ECS/Components/ParticleEmitterComponent.h"
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

[[noreturn]] void invalidScene(const std::string& message) {
    throw std::runtime_error("Invalid scene: " + message);
}

void expect(std::istream& input, const std::string_view expected) {
    std::string token;
    if (!(input >> token) || token != expected) {
        invalidScene("expected '" + std::string(expected) + "'");
    }
}

template<typename T>
T read(std::istream& input, const std::string_view description) {
    T value{};
    if (!(input >> value)) {
        invalidScene("could not read " + std::string(description));
    }
    return value;
}

float readFloat(std::istream& input, const std::string_view description) {
    const auto value = read<float>(input, description);
    if (!std::isfinite(value)) {
        invalidScene(std::string(description) + " must be finite");
    }
    return value;
}

std::size_t readCount(std::istream& input, const std::string_view description,
                      const std::size_t maximum) {
    const auto value = read<unsigned long long>(input, description);
    if (value > maximum) {
        invalidScene(std::string(description) + " exceeds the supported limit");
    }
    return static_cast<std::size_t>(value);
}

bool readBool(std::istream& input, const std::string_view description) {
    const int value = read<int>(input, description);
    if (value != 0 && value != 1) {
        invalidScene(std::string(description) + " must be 0 or 1");
    }
    return value != 0;
}

Vec3 readVec3(std::istream& input, const std::string_view description) {
    return {
        readFloat(input, description),
        readFloat(input, description),
        readFloat(input, description),
    };
}

Math::Color readColor(std::istream& input, const std::string_view description) {
    const float red = readFloat(input, description);
    const float green = readFloat(input, description);
    const float blue = readFloat(input, description);
    return Math::Color::from_rgb(red, green, blue);
}

Math::Color readColorRgba(std::istream& input, const std::string_view description) {
    return {readFloat(input, std::string(description) + " red"),
            readFloat(input, std::string(description) + " green"),
            readFloat(input, std::string(description) + " blue"),
            readFloat(input, std::string(description) + " alpha")};
}

void writeFloat(std::ostream& output, const float value) {
    char buffer[32];
    const auto result = std::to_chars(std::begin(buffer), std::end(buffer), value);
    if (result.ec != std::errc{}) {
        throw std::runtime_error("Could not format scene float");
    }
    output.write(buffer, result.ptr - buffer);
}

void writeVec3(std::ostream& output, const Vec3& value) {
    writeFloat(output, value.x()); output << ' ';
    writeFloat(output, value.y()); output << ' ';
    writeFloat(output, value.z());
}

void writeColor(std::ostream& output, const Math::Color& value) {
    writeFloat(output, value.r()); output << ' ';
    writeFloat(output, value.g()); output << ' ';
    writeFloat(output, value.b());
}

void writeColorRgba(std::ostream& output, const Math::Color& value) {
    writeFloat(output, value.r()); output << ' ';
    writeFloat(output, value.g()); output << ' ';
    writeFloat(output, value.b()); output << ' ';
    writeFloat(output, value.a());
}

void writeMaterial(std::ostream& output, const PBRMaterial& material) {
    writeColorRgba(output, material.baseColor);
    output << ' '; writeFloat(output, material.metallic); output << ' ';
    writeFloat(output, material.roughness); output << ' ';
    writeFloat(output, material.ambientOcclusion); output << ' ' << material.baseColorTexture << ' '
           << material.metallicRoughnessTexture << ' ' << material.normalTexture << ' ';
    writeFloat(output, material.normalScale);
    output << ' ' << static_cast<int>(material.alphaBlend) << ' '
           << static_cast<int>(material.doubleSided) << ' ';
    writeFloat(output, material.alphaCutoff);
}

PBRMaterial readMaterial(std::istream& input) {
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

void writeParticleEmitter(std::ostream& output, const Particles::ParticleEmitter& emitter) {
    writeVec3(output, emitter.position); output << ' ';
    writeVec3(output, emitter.minVelocity); output << ' ';
    writeVec3(output, emitter.maxVelocity); output << ' ';
    writeColorRgba(output, emitter.color); output << ' ';
    writeFloat(output, emitter.minLifeTime); output << ' ';
    writeFloat(output, emitter.maxLifeTime); output << ' ';
    writeFloat(output, emitter.minSize); output << ' ';
    writeFloat(output, emitter.maxSize); output << ' ';
    writeFloat(output, emitter.spawnRate);
}

Particles::ParticleEmitter readParticleEmitter(std::istream& input) {
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
    emitter.accumulator = 0.0f;
    if (emitter.minLifeTime < 0.0f || emitter.maxLifeTime < emitter.minLifeTime ||
        emitter.minSize < 0.0f || emitter.maxSize < emitter.minSize ||
        emitter.spawnRate < 0.0f) {
        invalidScene("particle emitter settings are invalid");
    }
    return emitter;
}

std::vector<Entity> sortedEntities(const Registry& registry) {
    std::vector<Entity> entities;
    entities.reserve(registry.size());
    registry.view<>([&entities](const Entity entity) {
        entities.push_back(entity);
    });
    std::ranges::sort(entities);
    return entities;
}

} // namespace

void SceneSerializer::save(const Scene& scene, const std::filesystem::path& path) {
    save(scene.registry, path);
}

void SceneSerializer::save(const Scene& scene, std::ostream& output) {
    save(scene.registry, output);
}

void SceneSerializer::load(Scene& scene, const std::filesystem::path& path) {
    load(scene.registry, path);
}

void SceneSerializer::load(Scene& scene, std::istream& input) {
    load(scene.registry, input);
}

void SceneSerializer::save(const Registry& registry,
                           const std::filesystem::path& path) {
    save(registry, path, 0);
}

void SceneSerializer::save(const Registry& registry,
                           const std::filesystem::path& path,
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

void SceneSerializer::save(const Registry& registry, std::ostream& output) {
    save(registry, output, 0);
}

void SceneSerializer::save(const Registry& registry, std::ostream& output,
                           const std::uint32_t msaaSamples) {
    std::ostringstream serialized;
    serialized.imbue(std::locale::classic());
    const std::vector<Entity> entities = sortedEntities(registry);
    std::vector<std::shared_ptr<const Mesh>> meshes;
    std::unordered_map<const Mesh*, std::size_t> meshIds;

    for (const Entity entity : entities) {
        if (!registry.has<MeshRenderer>(entity)) {
            continue;
        }
        const auto& renderer = registry.get<MeshRenderer>(entity);
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
        const Mesh& mesh = *meshes[meshId];
        serialized << "MESH " << meshId << ' ' << mesh.vertices.size() << ' '
                   << mesh.indices.size() << ' ' << mesh.materials.size() << ' '
                   << mesh.images.size() << '\n';
        for (const Vertex& vertex : mesh.vertices) {
            serialized << "VERTEX ";
            writeVec3(serialized, vertex.position);
            serialized << ' ';
            writeVec3(serialized, vertex.color);
            serialized << ' '; writeFloat(serialized, vertex.texCoord.x()); serialized << ' ';
            writeFloat(serialized, vertex.texCoord.y()); serialized << ' ';
            writeVec3(serialized, vertex.normal);
            serialized << ' '; writeFloat(serialized, vertex.tangent.x()); serialized << ' ';
            writeFloat(serialized, vertex.tangent.y()); serialized << ' ';
            writeFloat(serialized, vertex.tangent.z()); serialized << ' ';
            writeFloat(serialized, vertex.tangent.w());
            serialized << ' ' << vertex.materialIndex;
            serialized << '\n';
        }
        serialized << "INDICES";
        for (const std::uint32_t index : mesh.indices) {
            serialized << ' ' << index;
        }
        serialized << '\n';
        for (const PBRMaterial& material : mesh.materials) {
            serialized << "MATERIAL ";
            writeMaterial(serialized, material);
            serialized << '\n';
        }
        for (const Mesh::Image& image : mesh.images) {
            serialized << "IMAGE " << image.width << ' ' << image.height << ' '
                       << image.rgbaPixels.size() << '\n' << "PIXELS";
            for (const std::uint8_t pixel : image.rgbaPixels) serialized << ' ' << static_cast<unsigned>(pixel);
            serialized << '\n';
        }
    }

    serialized << "ENTITIES " << entities.size() << '\n';
    for (const Entity entity : entities) {
        serialized << "ENTITY\n";
        const UUID uuid = registry.has<UUIDComponent>(entity)
            ? registry.get<UUIDComponent>(entity).value : entity;
        const std::string name = registry.has<NameComponent>(entity)
            ? registry.get<NameComponent>(entity).value
            : "Entity " + std::to_string(entityIndex(entity));
        serialized << "IDENTITY " << uuid << ' ' << std::quoted(name) << '\n';
        if (registry.has<ParentComponent>(entity)) {
            serialized << "PARENT " << registry.get<ParentComponent>(entity).parent << '\n';
        }
        if (registry.has<Transform>(entity)) {
            const auto& transform = registry.get<Transform>(entity);
            serialized << "TRANSFORM ";
            writeVec3(serialized, transform.position);
            serialized << ' ';
            writeVec3(serialized, transform.rotation);
            serialized << ' ';
            writeVec3(serialized, transform.scale);
            serialized << '\n';
        }
        if (registry.has<MeshRenderer>(entity)) {
            const auto& renderer = registry.get<MeshRenderer>(entity);
            const long long meshId = renderer.mesh
                ? static_cast<long long>(meshIds.at(renderer.mesh.get()))
                : -1;
            serialized << "MESH_RENDERER " << meshId << ' ';
            writeMaterial(serialized, renderer.material);
            serialized << ' ' << static_cast<int>(renderer.castShadow) << ' '
                       << renderer.cullingBatch << '\n';
        }
        if (registry.has<LightComponent>(entity)) {
            const auto& light = registry.get<LightComponent>(entity);
            serialized << "LIGHT " << static_cast<int>(light.type) << ' ';
            writeColor(serialized, light.color);
            serialized << ' '; writeFloat(serialized, light.intensity);
            serialized << ' ' << static_cast<int>(light.enabled) << ' '
                       << static_cast<int>(light.castShadows) << '\n';
        }
        if (registry.has<CameraComponent>(entity)) {
            const auto& camera = registry.get<CameraComponent>(entity);
            serialized << "CAMERA " << static_cast<int>(camera.projection) << ' ';
            writeFloat(serialized, camera.fieldOfView); serialized << ' ';
            writeFloat(serialized, camera.orthographicSize); serialized << ' ';
            writeFloat(serialized, camera.nearClip); serialized << ' ';
            writeFloat(serialized, camera.farClip); serialized << ' ';
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
        if (registry.has<ScriptComponent>(entity)) {
            const auto& script = registry.get<ScriptComponent>(entity);
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

void SceneSerializer::load(Registry& registry,
                           const std::filesystem::path& path) {
    std::optional<std::uint32_t> ignoredMsaa;
    load(registry, path, ignoredMsaa);
}

void SceneSerializer::load(Registry& registry,
                           const std::filesystem::path& path,
                           std::optional<std::uint32_t>& msaaSamples) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Could not open scene for reading: " + path.string());
    }
    load(registry, input, msaaSamples);
}

void SceneSerializer::load(Registry& registry, std::istream& input) {
    std::optional<std::uint32_t> ignoredMsaa;
    load(registry, input, ignoredMsaa);
}

void SceneSerializer::load(Registry& registry, std::istream& input,
                           std::optional<std::uint32_t>& msaaSamples) {
    msaaSamples.reset();
    input.imbue(std::locale::classic());
    expect(input, "GAMENGINE_SCENE");
    const auto version = read<unsigned>(input, "format version");
    if (version != 3 && version != FormatVersion) {
        invalidScene("unsupported format version " + std::to_string(version));
    }

    std::string section;
    if (!(input >> section)) invalidScene("expected 'MESHES'");
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
    std::vector<std::shared_ptr<const Mesh>> meshes;
    meshes.reserve(meshCount);
    for (std::size_t meshIndex = 0; meshIndex < meshCount; ++meshIndex) {
        expect(input, "MESH");
        const std::size_t serializedId = readCount(input, "mesh id", MaxMeshes);
        if (serializedId != meshIndex) {
            invalidScene("mesh identifiers must be contiguous");
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
            vertex.tangent = {readFloat(input, "tangent x"), readFloat(input, "tangent y"),
                               readFloat(input, "tangent z"), readFloat(input, "tangent handedness")};
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
            if (value.width == 0 || value.height == 0 || pixelCount != static_cast<std::size_t>(value.width) * value.height * 4) {
                invalidScene("image dimensions do not match RGBA pixel data");
            }
            expect(input, "PIXELS");
            value.rgbaPixels.reserve(pixelCount);
            for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
                const auto byte = read<unsigned>(input, "image pixel");
                if (byte > 255) invalidScene("image pixel is outside byte range");
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
                if (uuid.value == NullUUID) invalidScene("object UUID cannot be zero");
                NameComponent name;
                if (!(input >> std::quoted(name.value)) || name.value.empty()) {
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
                if (parent == NullUUID) invalidScene("parent UUID cannot be zero");
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
                if (!(input >> std::quoted(script.className))) {
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
                    .color = readColorRgba(input, "color picker color")});
            } else if (component == "PARTICLE_EMITTER") {
                if (hasParticleEmitter) {
                    invalidScene("entity contains more than one ParticleEmitterComponent");
                }
                hasParticleEmitter = true;
                loaded.add<ParticleEmitterComponent>(entity,
                    ParticleEmitterComponent{.emitter = readParticleEmitter(input)});
            } else {
                invalidScene("unknown component '" + component + "'");
            }
        }
        if (version >= 4 && !hasIdentity) {
            invalidScene("entity is missing IDENTITY");
        }
        if (version == 3) {
            loaded.add<UUIDComponent>(entity, UUIDComponent{.value = createUUID()});
            loaded.add<NameComponent>(entity, NameComponent{.value = "Entity " + std::to_string(Engine::entityIndex(entity))});
        }
        // Scenes written before ParticleEmitterComponent was serialized may
        // still contain the preset's "Particle System" entity. Reconstruct
        // its emitter from the entity's existing transform and color picker.
        // New scenes always take the PARTICLE_EMITTER branch above.
        if (!hasParticleEmitter && loaded.has<NameComponent>(entity) &&
            loaded.get<NameComponent>(entity).value == "Particle System") {
            Particles::ParticleEmitter emitter;
            // These are the original Particle Scene preset values. Older
            // scene files did not serialize the emitter at all, so using the
            // generic ParticleEmitter defaults would noticeably shrink and
            // thin the restored effect.
            emitter.minVelocity = {-0.8f, 5.5f, -0.8f};
            emitter.maxVelocity = {0.8f, 9.0f, 0.8f};
            emitter.minLifeTime = 1.2f;
            emitter.maxLifeTime = 3.4f;
            emitter.minSize = 0.06f;
            emitter.maxSize = 0.16f;
            emitter.spawnRate = 900.0f;
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
    loaded.view<UUIDComponent>([&](const Entity entity, const UUIDComponent& uuid) {
        if (!entitiesByUuid.emplace(uuid.value, entity).second) {
            invalidScene("object UUID is duplicated");
        }
        if (loaded.has<ParentComponent>(entity)) {
            const UUID parent = loaded.get<ParentComponent>(entity).parent;
            if (parent == uuid.value) invalidScene("object cannot be its own parent");
            parentsByUuid.emplace(uuid.value, parent);
        }
    });
    for (const auto& [child, parent] : parentsByUuid) {
        if (!entitiesByUuid.contains(parent)) {
            invalidScene("parent UUID does not refer to an object in this scene");
        }
        std::unordered_set<UUID> visited;
        UUID current = child;
        auto parentIt = parentsByUuid.find(current);
        while (parentIt != parentsByUuid.end()) {
            current = parentIt->second;
            if (!visited.insert(current).second) invalidScene("parent hierarchy contains a cycle");
            parentIt = parentsByUuid.find(current);
        }
    }

    registry = std::move(loaded);
}

} // namespace Engine
