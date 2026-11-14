// Places real lights where a scene's geometry glows.
//
// Every emissive part of every mesh in the input scene becomes a point or spot
// light: a flat piece of glowing glass (a lamp cover, a fixture lens) a spot
// aimed along its normal, anything with volume (a globe, a bulb) a point
// light. Optionally sets a night sky, post-processing and a fixed preview
// camera, then writes a new scene; the input scene is never modified.
//
//   SceneLightBaker <project.project> <in.scene> <out.scene> [options]
//     --sky <asset-relative .hdr>    environment panorama
//     --exposure <ev>                exposure compensation (default -1)
//     --bloom <weight>               bloom strength (default 0.08)
//     --intensity <scale>            multiplies every light (default 1)
//     --camera <x y z pitch yaw>     replace the player camera for previews
#include "Engine/Assets/Gmesh.h"
#include "Engine/ECS/Components/MeshRendererComponent.h"
#include "Engine/Project.h"
#include "Engine/Renderer/Geometry/Mesh.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneEditor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
#include <numeric>
#include <string>
#include <tuple>
#include <vector>

namespace {
using namespace Engine;

struct Glow final {
    std::uint32_t material{};
    Vec3 center;
    Vec3 normalSum;
    double area{};
    Vec3 low{1e9F, 1e9F, 1e9F};
    Vec3 high{-1e9F, -1e9F, -1e9F};
};

int findRoot(std::vector<int>& parent, int x) {
    while (parent[x] != x)
        x = parent[x] = parent[parent[x]];
    return x;
}

Vec3 minimum(const Vec3& a, const Vec3& b) {
    return {std::min(a.x(), b.x()), std::min(a.y(), b.y()), std::min(a.z(), b.z())};
}
Vec3 maximum(const Vec3& a, const Vec3& b) {
    return {std::max(a.x(), b.x()), std::max(a.y(), b.y()), std::max(a.z(), b.z())};
}

// Connected components of each emissive section's welded triangles, then
// pieces of one fixture (within a few centimetres) merged together.
std::vector<Glow> findGlows(const Mesh& mesh) {
    std::vector<Glow> parts;
    for (const auto& section : mesh.renderSections) {
        const PBRMaterial& material = mesh.materials.at(section.materialIndex);
        if (material.emissiveIntensity <= 0.0F)
            continue;
        std::map<std::tuple<long, long, long>, int> weld;
        std::vector<int> corner;
        corner.reserve(section.indexCount);
        for (std::uint32_t i = 0; i < section.indexCount; ++i) {
            const Vec3& p = mesh.vertices[mesh.indices[section.firstIndex + i]].position;
            const std::tuple<long, long, long> key{std::lround(p.x() * 500.0F), std::lround(p.y() * 500.0F),
                                                   std::lround(p.z() * 500.0F)};
            corner.push_back(weld.try_emplace(key, static_cast<int>(weld.size())).first->second);
        }
        std::vector<int> parent(weld.size());
        std::iota(parent.begin(), parent.end(), 0);
        for (std::size_t t = 0; t + 2 < corner.size(); t += 3) {
            const int root = findRoot(parent, corner[t]);
            parent[findRoot(parent, corner[t + 1])] = root;
            parent[findRoot(parent, corner[t + 2])] = root;
        }
        std::map<int, Glow> components;
        for (std::size_t t = 0; t + 2 < corner.size(); t += 3) {
            Glow& glow = components.try_emplace(findRoot(parent, corner[t]), Glow{section.materialIndex}).first->second;
            const Vec3& a = mesh.vertices[mesh.indices[section.firstIndex + t]].position;
            const Vec3& b = mesh.vertices[mesh.indices[section.firstIndex + t + 1]].position;
            const Vec3& c = mesh.vertices[mesh.indices[section.firstIndex + t + 2]].position;
            const Vec3 n = cross(b - a, c - a);
            const double area = 0.5 * n.length();
            glow.center += (a + b + c) * static_cast<float>(area / 3.0);
            glow.normalSum += n * 0.5F;
            glow.area += area;
            for (const Vec3* v : {&a, &b, &c}) {
                glow.low = minimum(glow.low, *v);
                glow.high = maximum(glow.high, *v);
            }
        }
        for (auto& [root, glow] : components)
            if (glow.area > 0.0)
                parts.push_back(glow);
    }
    constexpr float MergeDistance = 0.05F;
    std::vector<int> parent(parts.size());
    std::iota(parent.begin(), parent.end(), 0);
    for (std::size_t i = 0; i < parts.size(); ++i)
        for (std::size_t j = i + 1; j < parts.size(); ++j) {
            if (parts[i].material != parts[j].material)
                continue;
            bool overlap = true;
            for (int k = 0; k < 3; ++k)
                overlap = overlap && parts[i].low.native()[k] - MergeDistance <= parts[j].high.native()[k] &&
                          parts[j].low.native()[k] - MergeDistance <= parts[i].high.native()[k];
            if (overlap)
                parent[findRoot(parent, static_cast<int>(j))] = findRoot(parent, static_cast<int>(i));
        }
    std::map<int, Glow> merged;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        Glow& m = merged.try_emplace(findRoot(parent, static_cast<int>(i)), Glow{parts[i].material}).first->second;
        m.center += parts[i].center;
        m.normalSum += parts[i].normalSum;
        m.area += parts[i].area;
        m.low = minimum(m.low, parts[i].low);
        m.high = maximum(m.high, parts[i].high);
    }
    std::vector<Glow> glows;
    for (auto& [root, glow] : merged) {
        glow.center = glow.center * static_cast<float>(1.0 / glow.area);
        glows.push_back(glow);
    }
    return glows;
}

std::string lowercase(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Warm incandescent unless the material's name says otherwise.
Math::Color glowColor(const std::string& materialName) {
    const std::string name = lowercase(materialName);
    const std::pair<const char*, Math::Color> named[] = {
        {"orange", {1.0F, 0.55F, 0.2F}}, {"blue", {0.3F, 0.5F, 1.0F}}, {"pink", {1.0F, 0.45F, 0.7F}},
        {"red", {1.0F, 0.25F, 0.18F}},   {"green", {0.35F, 1.0F, 0.4F}}, {"pharmacy", {0.3F, 1.0F, 0.4F}},
        {"white", {1.0F, 0.88F, 0.7F}}};
    for (const auto& [word, color] : named)
        if (name.find(word) != std::string::npos)
            return color;
    return {1.0F, 0.72F, 0.42F};
}

std::string materialName(const Mesh& mesh, const PBRMaterial& material) {
    const std::int32_t texture = material.emissiveTexture >= 0 ? material.emissiveTexture : material.baseColorTexture;
    return texture >= 0 ? mesh.images.at(static_cast<std::size_t>(texture)).cookedPath.filename().string() : "";
}

// Point on the emitting side and the light it should carry.
std::pair<LightComponent, Vec3> lightFor(const Mesh& mesh, const Glow& glow, const float scale) {
    const Vec3 size = glow.high - glow.low;
    const float extent = std::max({size.x(), size.y(), size.z()});
    const float normalLength = glow.normalSum.length();
    // A flat emitter's triangle normals agree; a closed volume's cancel.
    const bool flat = normalLength > 0.6F * static_cast<float>(glow.area);
    LightComponent light;
    light.castShadows = true;
    light.color = glowColor(materialName(mesh, mesh.materials.at(glow.material)));
    Vec3 position = glow.center;
    if (flat && extent < 0.8F) {
        // A lamp lens: light leaves its front face.
        const Vec3 normal = glow.normalSum * (1.0F / normalLength);
        light.type = LightType::Spot;
        light.intensity = 14.0F;
        light.range = 12.0F;
        light.innerConeAngle = 10.0F;
        light.outerConeAngle = 80.0F;
        light.mainLight = false;
        position = glow.center + normal * 0.05F;
        // Spot direction is stored separately in the transform (see main).
        light.color = glowColor(materialName(mesh, mesh.materials.at(glow.material)));
    } else if (extent < 0.2F) {
        // A small emitter. String-light bulbs get a brighter coloured pool on
        // the facade and paving below; other small glowing parts (lantern
        // panes, fixture details) stay dim, since the lantern's own light
        // already carries its output. The range stays short so dense strings
        // do not lengthen every cluster's light list.
        light.type = LightType::Point;
        const bool stringBulb =
            lowercase(materialName(mesh, mesh.materials.at(glow.material))).find("stringlights") != std::string::npos;
        light.intensity = stringBulb ? 10.0F : 4.0F;
        light.range = 6.0F;
    } else if (extent < 0.8F) {
        // A lamp globe or lantern.
        light.type = LightType::Point;
        light.intensity = 28.0F;
        light.range = 16.0F;
    } else {
        // A large glowing sign.
        light.type = LightType::Point;
        light.intensity = 6.0F;
        light.range = 6.0F;
    }
    light.intensity *= scale;
    return {light, position};
}

// Euler rotation (degrees) turning the light's -Z forward onto `direction`.
Vec3 aim(const Vec3& direction) {
    const float pitch = std::asin(std::clamp(direction.y(), -1.0F, 1.0F));
    const float yaw = std::atan2(-direction.x(), -direction.z());
    constexpr float toDegrees = 57.2957795F;
    return {pitch * toDegrees, yaw * toDegrees, 0.0F};
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 4) {
            std::cerr << "usage: SceneLightBaker <project> <in.scene> <out.scene> [--sky hdr] [--exposure ev] "
                         "[--bloom w] [--intensity s] [--camera x y z pitch yaw]\n";
            return 2;
        }
        const Project project = Project::load(argv[1]);
        const std::filesystem::path input = project.resolve(argv[2]);
        const std::filesystem::path output = project.resolve(argv[3]);
        std::string sky;
        float exposure = -1.0F, bloom = 0.08F, scale = 1.0F;
        std::vector<float> camera;
        for (int i = 4; i < argc; ++i) {
            const std::string option = argv[i];
            const auto next = [&]() -> std::string {
                if (++i >= argc)
                    throw std::runtime_error(option + " needs a value");
                return argv[i];
            };
            if (option == "--sky")
                sky = next();
            else if (option == "--exposure")
                exposure = std::stof(next());
            else if (option == "--bloom")
                bloom = std::stof(next());
            else if (option == "--intensity")
                scale = std::stof(next());
            else if (option == "--camera")
                for (int k = 0; k < 5; ++k)
                    camera.push_back(std::stof(next()));
            else
                throw std::runtime_error("unknown option " + option);
        }

        Scene scene;
        scene.load(input);
        // Collect meshes first: creating lights below changes the registry.
        std::vector<std::pair<std::filesystem::path, Mat4>> placements;
        scene.editor().template view<MeshRendererComponent, Transform>(
            [&](const Entity, const MeshRendererComponent& renderer, const Transform& transform) {
                const auto& resource = renderer.mesh.resource();
                if (resource && !resource->sourcePath.empty())
                    placements.emplace_back(resource->sourcePath, transform.worldMatrix());
            });
        const Actor group = scene.createActor("Baked Lights");
        std::size_t count = 0;
        std::map<std::filesystem::path, std::vector<Glow>> cache;
        for (const auto& [path, world] : placements) {
            const std::shared_ptr<const Mesh> mesh = Assets::load_gmesh(path);
            if (!mesh)
                continue;
            auto [it, inserted] = cache.try_emplace(path);
            if (inserted)
                it->second = findGlows(*mesh);
            for (const Glow& glow : it->second) {
                auto [light, local] = lightFor(*mesh, glow, scale);
                const glm::vec4 position = world.native() * glm::vec4{local.native(), 1.0F};
                Actor actor = scene.createLight(light.type == LightType::Spot ? "Lamp Spot" : "Lamp Light", light);
                actor.setPosition(Vec3{position.x, position.y, position.z});
                if (light.type == LightType::Spot) {
                    const glm::vec3 normal = glm::normalize(glm::vec3(world.native() *
                                                                      glm::vec4{glow.normalSum.native(), 0.0F}));
                    actor.setRotation(aim(Vec3{normal}));
                }
                actor.setParent(group);
                ++count;
            }
        }
        if (!sky.empty())
            scene.setEnvironmentEquirectangular(sky);
        PostProcessSettings post;
        post.exposureCompensation = exposure;
        post.minLuminanceEv = -9.0F;
        post.temperature = 0.3F;
        post.saturation = 1.15F;
        post.contrast = 1.05F;
        post.vignette = 0.2F;
        post.bloom = bloom;
        scene.setPostProcess(post);
        if (camera.size() == 5) {
            if (Actor previous = scene.findActor("Camera"); previous.valid())
                scene.destroy(previous);
            CameraComponent component;
            component.fieldOfView = 68.0F;
            Actor preview = scene.createCamera("Preview Camera", component);
            preview.setPosition(Vec3{camera[0], camera[1], camera[2]});
            preview.setRotation(Vec3{camera[3], camera[4], 0.0F});
        }
        scene.save(output);
        std::cout << "Saved " << output.string() << " with " << count << " lights\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SceneLightBaker: " << error.what() << '\n';
        return 1;
    }
}
