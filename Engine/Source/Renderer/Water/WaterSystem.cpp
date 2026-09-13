#include "Engine/Renderer/Water/WaterSystem.h"

#include "Engine/ECS/Components/MeshRendererComponent.h"
#include "Engine/ECS/Components/TransformComponent.h"
#include "Engine/Scene/SceneEditor.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace Engine {
namespace {
constexpr std::uint32_t ClipmapResolution = 32;
constexpr float ClipmapExtents[] = {50.0F, 150.0F, 500.0F, 2000.0F, 8000.0F};

void addQuad(Mesh& mesh, const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d) {
    const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
    WaterSystem::addWaterVertex(mesh, a, {0.0F, 0.0F}); WaterSystem::addWaterVertex(mesh, b, {1.0F, 0.0F});
    WaterSystem::addWaterVertex(mesh, c, {1.0F, 1.0F}); WaterSystem::addWaterVertex(mesh, d, {0.0F, 1.0F});
    mesh.indices.insert(mesh.indices.end(), {first, first + 1U, first + 2U, first, first + 2U, first + 3U});
}
} // namespace

void WaterSystem::addWaterVertex(Mesh& mesh, const Vec3& position, const Vec2& uv) {
    mesh.vertices.push_back({.position = position, .color = {1.0F, 1.0F, 1.0F}, .texCoord = uv,
                             .normal = {0.0F, 1.0F, 0.0F}, .tangent = {1.0F, 0.0F, 0.0F, 1.0F}});
}

Mesh WaterSystem::buildOceanClipmap() {
    Mesh mesh;
    for (std::uint32_t level = 0; level < std::size(ClipmapExtents); ++level) {
        const float outer = ClipmapExtents[level];
        const float inner = level == 0 ? 0.0F : ClipmapExtents[level - 1U];
        const float cell = (2.0F * outer) / static_cast<float>(ClipmapResolution);
        for (std::uint32_t z = 0; z < ClipmapResolution; ++z) for (std::uint32_t x = 0; x < ClipmapResolution; ++x) {
            const float minX = -outer + cell * static_cast<float>(x); const float minZ = -outer + cell * static_cast<float>(z);
            const float maxX = minX + cell; const float maxZ = minZ + cell;
            const float centerX = (minX + maxX) * 0.5F; const float centerZ = (minZ + maxZ) * 0.5F;
            if (level != 0 && std::abs(centerX) < inner && std::abs(centerZ) < inner) continue;
            addQuad(mesh, {minX, 0.0F, minZ}, {maxX, 0.0F, minZ}, {maxX, 0.0F, maxZ}, {minX, 0.0F, maxZ});
        }
    }
    return mesh;
}

Mesh WaterSystem::buildLake(const WaterBodyComponent& water) {
    if (water.lakeBoundary.size() < 3) throw std::invalid_argument("Lake requires at least three boundary points");
    Mesh mesh;
    // A triangle fan is deterministic and valid for convex editor boundaries.
    // Concave boundaries will be switched to editor-side ear clipping with holes.
    for (const Vec3& point : water.lakeBoundary) addWaterVertex(mesh, point, {point.x(), point.z()});
    for (std::uint32_t i = 1; i + 1 < mesh.vertices.size(); ++i)
        mesh.indices.insert(mesh.indices.end(), {0U, i, i + 1U});
    return mesh;
}

Mesh WaterSystem::buildRiver(const WaterBodyComponent& water) {
    if (water.riverSpline.size() < 2) throw std::invalid_argument("River requires at least two spline points");
    Mesh mesh;
    for (std::size_t i = 0; i < water.riverSpline.size(); ++i) {
        const RiverSplinePoint& point = water.riverSpline[i];
        const Vec3& before = water.riverSpline[i == 0 ? i : i - 1U].position;
        const Vec3& after = water.riverSpline[std::min(i + 1U, water.riverSpline.size() - 1U)].position;
        Vec2 direction{after.x() - before.x(), after.z() - before.z()};
        if (direction.length() < 1.0e-4F) direction = {1.0F, 0.0F}; else direction = direction.normalized();
        const Vec2 side{-direction.y(), direction.x()}; const float halfWidth = point.width * 0.5F;
        addWaterVertex(mesh, {point.position.x() - side.x() * halfWidth, point.position.y(), point.position.z() - side.y() * halfWidth}, {0.0F, static_cast<float>(i)});
        addWaterVertex(mesh, {point.position.x() + side.x() * halfWidth, point.position.y(), point.position.z() + side.y() * halfWidth}, {1.0F, static_cast<float>(i)});
    }
    for (std::uint32_t i = 0; i + 1 < water.riverSpline.size(); ++i) {
        const uint32_t first = i * 2U;
        mesh.indices.insert(mesh.indices.end(), {first, first + 1U, first + 3U, first, first + 3U, first + 2U});
    }
    return mesh;
}

void WaterSystem::rebuild(Registry& registry, const Entity entity) const {
    if (!registry.has<WaterBodyComponent>(entity)) throw std::invalid_argument("Entity has no WaterBodyComponent");
    const WaterBodyComponent& water = registry.get<WaterBodyComponent>(entity);
    Mesh mesh = buildMesh(water);
    auto source = std::make_shared<Mesh>(std::move(mesh));
    if (!registry.has<MeshRendererComponent>(entity)) registry.add<MeshRendererComponent>(entity);
    registry.modify<MeshRendererComponent>(entity, [&](MeshRendererComponent& renderer) {
        renderer.mesh = MeshHandle{std::move(source)}; renderer.materialOverride = true;
        renderer.material.shader = MaterialShader::Water;
        renderer.material.water = {
            .shallowColor = water.shallowColor,
            .deepColor = water.deepColor,
            .absorptionCoefficient = water.absorptionCoefficient,
            .scatteringCoefficient = water.scatteringCoefficient,
            .roughness = water.roughness,
            .ior = water.ior,
            .refractionStrength = water.refractionStrength,
            .normalStrength = water.normalStrength,
            .foamIntensity = water.foamIntensity,
            .foamThreshold = water.foamThreshold,
            .maxVisibleDepth = water.maxDepth,
            .normalMap = water.normalMap,
            .foamTexture = water.foamTexture,
            .flowMap = water.flowMap,
            .enableSSR = water.enableSSR,
            .enableCaustics = water.enableCaustics,
            .enableUnderwater = water.enableUnderwater,
            .waves = water.waves,
            .waveCount = water.waveCount,
        };
        // Water is composited as a surface; it must not produce a shadow-map
        // receiver/caster entry from its displaced visual mesh.
        renderer.castShadow = false;
    });
}

void WaterSystem::rebuild(SceneEditor& editor, const Entity entity) const {
    if (!editor.has<WaterBodyComponent>(entity)) throw std::invalid_argument("Entity has no WaterBodyComponent");
    const WaterBodyComponent& water = editor.read<WaterBodyComponent>(entity);
    Mesh mesh = buildMesh(water);
    auto source = std::make_shared<Mesh>(std::move(mesh));
    if (!editor.has<MeshRendererComponent>(entity)) editor.add<MeshRendererComponent>(entity);
    editor.patch<MeshRendererComponent>(entity, [&](MeshRendererComponent& renderer) {
        renderer.mesh = MeshHandle{std::move(source)}; renderer.materialOverride = true;
        renderer.material.shader = MaterialShader::Water;
        renderer.material.water = {
            .shallowColor = water.shallowColor, .deepColor = water.deepColor,
            .absorptionCoefficient = water.absorptionCoefficient, .scatteringCoefficient = water.scatteringCoefficient,
            .roughness = water.roughness, .ior = water.ior, .refractionStrength = water.refractionStrength,
            .normalStrength = water.normalStrength, .foamIntensity = water.foamIntensity,
            .foamThreshold = water.foamThreshold, .maxVisibleDepth = water.maxDepth,
            .normalMap = water.normalMap, .foamTexture = water.foamTexture, .flowMap = water.flowMap,
            .enableSSR = water.enableSSR, .enableCaustics = water.enableCaustics,
            .enableUnderwater = water.enableUnderwater, .waves = water.waves, .waveCount = water.waveCount,
        };
        renderer.castShadow = false;
    });
}

Mesh WaterSystem::buildMesh(const WaterBodyComponent& water) {
    return water.type == WaterBodyType::Ocean ? buildOceanClipmap() :
           water.type == WaterBodyType::Lake ? buildLake(water) : buildRiver(water);
}

void WaterSystem::updateOceans(Registry& registry, const Vec3& cameraPosition) const {
    registry.view<WaterBodyComponent, TransformComponent>([&](const Entity entity, const WaterBodyComponent& water, const TransformComponent&) {
        if (water.type != WaterBodyType::Ocean) return;
        const float snappedX = std::floor(cameraPosition.x() / 3.125F) * 3.125F;
        const float snappedZ = std::floor(cameraPosition.z() / 3.125F) * 3.125F;
        const TransformComponent& transform = registry.get<TransformComponent>(entity);
        if (transform.position.x() == snappedX && transform.position.z() == snappedZ) return;
        registry.modify<TransformComponent>(entity, [&](TransformComponent& transform) {
            transform.position.setX(snappedX);
            transform.position.setZ(snappedZ);
        });
    });
}
} // namespace Engine
