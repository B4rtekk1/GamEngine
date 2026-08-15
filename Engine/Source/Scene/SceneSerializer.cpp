#include "Engine/Scene/SceneSerializer.h"

#include "Engine/Core/Transform.h"
#include "Engine/ECS/Components/CameraComponent.h"
#include "Engine/ECS/Registry.h"
#include "Engine/Renderer/MeshRenderer.h"
#include "Engine/Scene/Components/LightComponent.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
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

void writeVec3(std::ostream& output, const Vec3& value) {
    output << value.x() << ' ' << value.y() << ' ' << value.z();
}

void writeColor(std::ostream& output, const Math::Color& value) {
    output << value.r() << ' ' << value.g() << ' ' << value.b();
}

std::vector<Entity> sortedEntities(const Registry& registry) {
    std::vector<Entity> entities;
    registry.view<>([&entities](const Entity entity) {
        entities.push_back(entity);
    });
    std::ranges::sort(entities);
    return entities;
}

} // namespace

void SceneSerializer::save(const Registry& registry,
                           const std::filesystem::path& path) {
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("Could not open scene for writing: " + path.string());
    }
    save(registry, output);
    if (!output) {
        throw std::runtime_error("Could not finish writing scene: " + path.string());
    }
}

void SceneSerializer::save(const Registry& registry, std::ostream& output) {
    std::ostringstream serialized;
    serialized.imbue(std::locale::classic());
    serialized << std::setprecision(std::numeric_limits<float>::max_digits10);

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

    serialized << "GAMENGINE_SCENE " << FormatVersion << '\n';
    serialized << "MESHES " << meshes.size() << '\n';
    for (std::size_t meshId = 0; meshId < meshes.size(); ++meshId) {
        const Mesh& mesh = *meshes[meshId];
        serialized << "MESH " << meshId << ' ' << mesh.vertices.size() << ' '
                   << mesh.indices.size() << '\n';
        for (const Vertex& vertex : mesh.vertices) {
            serialized << "VERTEX ";
            writeVec3(serialized, vertex.position);
            serialized << ' ';
            writeVec3(serialized, vertex.color);
            serialized << ' ' << vertex.texCoord.x() << ' ' << vertex.texCoord.y() << ' ';
            writeVec3(serialized, vertex.normal);
            serialized << '\n';
        }
        serialized << "INDICES";
        for (const std::uint32_t index : mesh.indices) {
            serialized << ' ' << index;
        }
        serialized << '\n';
    }

    serialized << "ENTITIES " << entities.size() << '\n';
    for (const Entity entity : entities) {
        serialized << "ENTITY\n";
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
            writeColor(serialized, renderer.material.baseColor);
            serialized << ' ' << renderer.material.metallic << ' '
                       << renderer.material.roughness << ' '
                       << renderer.material.ambientOcclusion << ' '
                       << static_cast<int>(renderer.castShadow) << '\n';
        }
        if (registry.has<LightComponent>(entity)) {
            const auto& light = registry.get<LightComponent>(entity);
            serialized << "LIGHT " << static_cast<int>(light.type) << ' ';
            writeColor(serialized, light.color);
            serialized << ' ' << light.intensity << ' '
                       << static_cast<int>(light.enabled) << ' '
                       << static_cast<int>(light.castShadows) << '\n';
        }
        if (registry.has<CameraComponent>(entity)) {
            const auto& camera = registry.get<CameraComponent>(entity);
            serialized << "CAMERA " << static_cast<int>(camera.projection) << ' '
                       << camera.fieldOfView << ' ' << camera.orthographicSize << ' '
                       << camera.nearClip << ' ' << camera.farClip << ' '
                       << camera.aspectRatio << ' ' << static_cast<int>(camera.primary) << '\n';
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
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Could not open scene for reading: " + path.string());
    }
    load(registry, input);
}

void SceneSerializer::load(Registry& registry, std::istream& input) {
    input.imbue(std::locale::classic());
    expect(input, "GAMENGINE_SCENE");
    const auto version = read<unsigned>(input, "format version");
    if (version != FormatVersion) {
        invalidScene("unsupported format version " + std::to_string(version));
    }

    expect(input, "MESHES");
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

        while (true) {
            const auto component = read<std::string>(input, "component name");
            if (component == "END_ENTITY") {
                break;
            }
            if (component == "TRANSFORM") {
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
                renderer.material.baseColor = readColor(input, "material base color");
                renderer.material.metallic = readFloat(input, "material metallic");
                renderer.material.roughness = readFloat(input, "material roughness");
                renderer.material.ambientOcclusion =
                    readFloat(input, "material ambient occlusion");
                renderer.castShadow = readBool(input, "cast-shadow flag");
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
            } else {
                invalidScene("unknown component '" + component + "'");
            }
        }
    }

    expect(input, "END_SCENE");
    std::string trailing;
    if (input >> trailing) {
        invalidScene("unexpected data after END_SCENE");
    }

    registry = std::move(loaded);
}

} // namespace Engine
