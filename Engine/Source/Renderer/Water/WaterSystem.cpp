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
        // Every level is exactly twice the previous one.  With a fixed 32x32 grid,
        // the inner boundary of level N therefore lies on vertices of level N-1.
        constexpr float ClipmapExtents[] = {
            50.0F, 100.0F, 200.0F, 400.0F, 800.0F,
            1600.0F, 3200.0F, 6400.0F, 12800.0F,
        };

        void addQuad(Mesh &mesh, const Vec3 &a, const Vec3 &b, const Vec3 &c, const Vec3 &d) {
            const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
            WaterSystem::addWaterVertex(mesh, a, {0.0F, 0.0F});
            WaterSystem::addWaterVertex(mesh, b, {1.0F, 0.0F});
            WaterSystem::addWaterVertex(mesh, c, {1.0F, 1.0F});
            WaterSystem::addWaterVertex(mesh, d, {0.0F, 1.0F});
            mesh.indices.insert(mesh.indices.end(), {first, first + 1U, first + 2U, first, first + 2U, first + 3U});
        }

        enum class StitchEdge { Bottom, Right, Top, Left };

        void addStitchedQuad(Mesh &mesh, const Vec3 &a, const Vec3 &b, const Vec3 &c,
                             const Vec3 &d, const StitchEdge edge) {
            const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
            const Vec3 midpoint = edge == StitchEdge::Bottom
                                      ? (a + b) * 0.5F
                                      : edge == StitchEdge::Right
                                            ? (b + c) * 0.5F
                                            : edge == StitchEdge::Top
                                                  ? (c + d) * 0.5F
                                                  : (d + a) * 0.5F;
            WaterSystem::addWaterVertex(mesh, a, {0.0F, 0.0F});
            WaterSystem::addWaterVertex(mesh, b, {1.0F, 0.0F});
            WaterSystem::addWaterVertex(mesh, c, {1.0F, 1.0F});
            WaterSystem::addWaterVertex(mesh, d, {0.0F, 1.0F});
            WaterSystem::addWaterVertex(mesh, midpoint, {0.5F, 0.5F});
            const std::uint32_t A = first, B = first + 1U, C = first + 2U, D = first + 3U, M = first + 4U;
            switch (edge) {
                case StitchEdge::Bottom: mesh.indices.insert(mesh.indices.end(), {A, M, D, M, C, D, M, B, C});
                    break;
                case StitchEdge::Right: mesh.indices.insert(mesh.indices.end(), {B, M, A, M, D, A, M, C, D});
                    break;
                case StitchEdge::Top: mesh.indices.insert(mesh.indices.end(), {D, M, A, M, B, A, M, C, B});
                    break;
                case StitchEdge::Left: mesh.indices.insert(mesh.indices.end(), {A, M, B, M, C, B, M, D, C});
                    break;
            }
        }
    } // namespace

    void WaterSystem::addWaterVertex(Mesh &mesh, const Vec3 &position, const Vec2 &uv) {
        mesh.vertices.push_back({
            .position = position, .color = {1.0F, 1.0F, 1.0F}, .texCoord = uv,
            .normal = {0.0F, 1.0F, 0.0F}, .tangent = {1.0F, 0.0F, 0.0F, 1.0F}
        });
    }

    Mesh WaterSystem::buildOceanClipmap() {
        Mesh mesh;
        for (std::uint32_t level = 0; level < std::size(ClipmapExtents); ++level) {
            const float outer = ClipmapExtents[level];
            const float inner = level == 0 ? 0.0F : ClipmapExtents[level - 1U];
            const float cell = (2.0F * outer) / static_cast<float>(ClipmapResolution);
            for (std::uint32_t z = 0; z < ClipmapResolution; ++z)
                for (std::uint32_t x = 0; x < ClipmapResolution; ++x) {
                    const float minX = -outer + cell * static_cast<float>(x);
                    const float minZ = -outer + cell * static_cast<float>(z);
                    const float maxX = minX + cell;
                    const float maxZ = minZ + cell;
                    // The hole is selected from cell bounds, never its centre.  The
                    // power-of-two extents make this exact: level N's hole is the
                    // outer square of level N - 1, so there can be neither overlap
                    // nor a gap at the ring boundary.
                    if (level != 0 && minX >= -inner && maxX <= inner &&
                        minZ >= -inner && maxZ <= inner)
                        continue;
                    const Vec3 a{minX, 0.0F, minZ}, b{maxX, 0.0F, minZ};
                    const Vec3 c{maxX, 0.0F, maxZ}, d{minX, 0.0F, maxZ};
                    // Split each coarse cell touching the hole at the fine-level
                    // boundary vertex.  This is true index stitching, not a
                    // centre-based approximation or a decorative skirt.
                    if (level != 0 && std::abs(minZ - inner) < 1.0e-4F)
                        addStitchedQuad(mesh, a, b, c, d, StitchEdge::Bottom);
                    else if (level != 0 && std::abs(maxX + inner) < 1.0e-4F)
                        addStitchedQuad(mesh, a, b, c, d, StitchEdge::Right);
                    else if (level != 0 && std::abs(maxZ + inner) < 1.0e-4F)
                        addStitchedQuad(mesh, a, b, c, d, StitchEdge::Top);
                    else if (level != 0 && std::abs(minX - inner) < 1.0e-4F)
                        addStitchedQuad(mesh, a, b, c, d, StitchEdge::Left);
                    else
                        addQuad(mesh, a, b, c, d);
                }
        }
        return mesh;
    }

    Mesh WaterSystem::buildLake(const WaterBodyComponent &water) {
        if (water.lakeBoundary.size() < 3) throw std::invalid_argument("Lake requires at least three boundary points");
        Mesh mesh;
        // A triangle fan is deterministic and valid for convex editor boundaries.
        // Concave boundaries will be switched to editor-side ear clipping with holes.
        for (const Vec3 &point: water.lakeBoundary) addWaterVertex(mesh, point, {point.x(), point.z()});
        for (std::uint32_t i = 1; i + 1 < mesh.vertices.size(); ++i)
            mesh.indices.insert(mesh.indices.end(), {0U, i, i + 1U});
        return mesh;
    }

    Mesh WaterSystem::buildRiver(const WaterBodyComponent &water) {
        if (water.riverSpline.size() < 2) throw std::invalid_argument("River requires at least two spline points");
        Mesh mesh;
        for (std::size_t i = 0; i < water.riverSpline.size(); ++i) {
            const RiverSplinePoint &point = water.riverSpline[i];
            const Vec3 &before = water.riverSpline[i == 0 ? i : i - 1U].position;
            const Vec3 &after = water.riverSpline[std::min(i + 1U, water.riverSpline.size() - 1U)].position;
            Vec2 direction{after.x() - before.x(), after.z() - before.z()};
            if (direction.length() < 1.0e-4F) direction = {1.0F, 0.0F};
            else direction = direction.normalized();
            const Vec2 side{-direction.y(), direction.x()};
            const float halfWidth = point.width * 0.5F;
            addWaterVertex(mesh, {
                               point.position.x() - side.x() * halfWidth, point.position.y(),
                               point.position.z() - side.y() * halfWidth
                           }, {0.0F, static_cast<float>(i)});
            addWaterVertex(mesh, {
                               point.position.x() + side.x() * halfWidth, point.position.y(),
                               point.position.z() + side.y() * halfWidth
                           }, {1.0F, static_cast<float>(i)});
        }
        for (std::uint32_t i = 0; i + 1 < water.riverSpline.size(); ++i) {
            const uint32_t first = i * 2U;
            mesh.indices.insert(mesh.indices.end(), {first, first + 1U, first + 3U, first, first + 3U, first + 2U});
        }
        return mesh;
    }

    void WaterSystem::rebuild(Registry &registry, const Entity entity) const {
        if (!registry.has<WaterBodyComponent>(entity)) throw std::invalid_argument("Entity has no WaterBodyComponent");
        const WaterBodyComponent &water = registry.get<WaterBodyComponent>(entity);
        Mesh mesh = buildMesh(water);
        auto waves = water.waves;
        const auto waveCount = std::min(water.waveCount, static_cast<std::uint32_t>(waves.size()));
        for (std::uint32_t index = 0; index < waveCount; ++index)
            if (waves[index].direction.length() < 1.0e-4F) waves[index].direction = {1.0F, 0.0F};
        auto source = std::make_shared<Mesh>(std::move(mesh));
        if (!registry.has<MeshRendererComponent>(entity)) registry.add<MeshRendererComponent>(entity);
        registry.modify<MeshRendererComponent>(entity, [&](MeshRendererComponent &renderer) {
            renderer.mesh = MeshHandle{std::move(source)};
            renderer.materialOverride = true;
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
                .waves = waves,
                .waveCount = waveCount,
            };
            // Water is composited as a surface; it must not produce a shadow-map
            // receiver/caster entry from its displaced visual mesh.
            renderer.castShadow = false;
        });
    }

    void WaterSystem::rebuild(SceneEditor &editor, const Entity entity) const {
        if (!editor.has<WaterBodyComponent>(entity)) throw std::invalid_argument("Entity has no WaterBodyComponent");
        const WaterBodyComponent &water = editor.read<WaterBodyComponent>(entity);
        Mesh mesh = buildMesh(water);
        auto waves = water.waves;
        const auto waveCount = std::min(water.waveCount, static_cast<std::uint32_t>(waves.size()));
        for (std::uint32_t index = 0; index < waveCount; ++index)
            if (waves[index].direction.length() < 1.0e-4F) waves[index].direction = {1.0F, 0.0F};
        auto source = std::make_shared<Mesh>(std::move(mesh));
        if (!editor.has<MeshRendererComponent>(entity)) editor.add<MeshRendererComponent>(entity);
        editor.patch<MeshRendererComponent>(entity, [&](MeshRendererComponent &renderer) {
            renderer.mesh = MeshHandle{std::move(source)};
            renderer.materialOverride = true;
            renderer.material.shader = MaterialShader::Water;
            renderer.material.water = {
                .shallowColor = water.shallowColor, .deepColor = water.deepColor,
                .absorptionCoefficient = water.absorptionCoefficient,
                .scatteringCoefficient = water.scatteringCoefficient,
                .roughness = water.roughness, .ior = water.ior, .refractionStrength = water.refractionStrength,
                .normalStrength = water.normalStrength, .foamIntensity = water.foamIntensity,
                .foamThreshold = water.foamThreshold, .maxVisibleDepth = water.maxDepth,
                .normalMap = water.normalMap, .foamTexture = water.foamTexture, .flowMap = water.flowMap,
                .enableSSR = water.enableSSR, .enableCaustics = water.enableCaustics,
                .enableUnderwater = water.enableUnderwater, .waves = waves, .waveCount = waveCount,
            };
            renderer.castShadow = false;
        });
    }

    Mesh WaterSystem::buildMesh(const WaterBodyComponent &water) {
        return water.type == WaterBodyType::Ocean
                   ? buildOceanClipmap()
                   : water.type == WaterBodyType::Lake
                         ? buildLake(water)
                         : buildRiver(water);
    }

    void WaterSystem::updateOceans(Registry &registry, const Vec3 &cameraPosition) const {
        registry.view<WaterBodyComponent, TransformComponent>(
            [&](const Entity entity, const WaterBodyComponent &water, const TransformComponent &) {
                if (water.type != WaterBodyType::Ocean) return;
                const float snappedX = std::floor(cameraPosition.x() / 3.125F) * 3.125F;
                const float snappedZ = std::floor(cameraPosition.z() / 3.125F) * 3.125F;
                const TransformComponent &transform = registry.get<TransformComponent>(entity);
                if (transform.position.x() == snappedX && transform.position.z() == snappedZ) return;
                registry.modify<TransformComponent>(entity, [&](TransformComponent &transform) {
                    transform.position.setX(snappedX);
                    transform.position.setZ(snappedZ);
                });
            });
    }
} // namespace Engine
