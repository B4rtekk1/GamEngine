#include "Engine/Renderer/Water/WaterSystem.h"

#include "Engine/ECS/Components/MeshRendererComponent.h"
#include "Engine/ECS/Components/TransformComponent.h"
#include "Engine/Scene/SceneEditor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <stdexcept>

namespace Engine {
    namespace {
        // Virtual-water geometry layout.  A level remains a 64x64-cell clipmap,
        // but is expressed as an 8x8 array of independently addressable 8x8
        // pages.  The current mesh path still submits the pages as one ocean
        // mesh; keeping this layout explicit is what lets the GPU page-list path
        // replace that submission without changing the surface function.
        constexpr std::uint32_t WaterPagesPerAxis = 8;
        constexpr std::uint32_t WaterPageCells = 8;
        constexpr std::uint32_t ClipmapResolution = WaterPagesPerAxis * WaterPageCells;
        static_assert(ClipmapResolution == 64);
        // Every level is exactly twice the previous one. With the fixed grid,
        // the inner boundary of level N lies on vertices of level N - 1.
        constexpr float ClipmapExtents[] = {
            50.0F, 100.0F, 200.0F, 400.0F, 800.0F,
            1600.0F, 3200.0F, 6400.0F, 12800.0F,
        };

        float samplingCellSize(const Vec3 &position, const float outer, const float cell,
                               const std::uint32_t level) {
            // The inner edge of the next ring uses its own cell size.  Give the
            // matching outer-edge vertices in this ring that same spectral LOD,
            // so filtered Gerstner displacement remains watertight at the seam.
            constexpr float edgeEpsilon = 1.0e-4F;
            if (level + 1U < std::size(ClipmapExtents) &&
                (std::abs(std::abs(position.x()) - outer) < edgeEpsilon ||
                 std::abs(std::abs(position.z()) - outer) < edgeEpsilon))
                return cell * 2.0F;
            return cell;
        }

        enum class StitchEdge { Bottom, Right, Top, Left };

        void addStitchedCellIndices(Mesh &mesh, const std::uint32_t a, const std::uint32_t b,
                                    const std::uint32_t c, const std::uint32_t d,
                                    const std::uint32_t midpoint, const StitchEdge edge) {
            switch (edge) {
                case StitchEdge::Bottom: mesh.indices.insert(mesh.indices.end(), {a, midpoint, d, midpoint, c, d, midpoint, b, c});
                    break;
                case StitchEdge::Right: mesh.indices.insert(mesh.indices.end(), {b, midpoint, a, midpoint, d, a, midpoint, c, d});
                    break;
                case StitchEdge::Top: mesh.indices.insert(mesh.indices.end(), {d, midpoint, a, midpoint, b, a, midpoint, c, b});
                    break;
                case StitchEdge::Left: mesh.indices.insert(mesh.indices.end(), {a, midpoint, b, midpoint, c, b, midpoint, d, c});
                    break;
            }
        }

        [[nodiscard]] bool pageIsInactive(const std::uint32_t level, const std::uint32_t pageX,
                                          const std::uint32_t pageZ) {
            // The 4x4 centre of every outer level is fully covered by its
            // preceding (finer) level. It is a logical virtual slot, but has
            // no geometry and therefore costs no vertex work.
            return level != 0U && pageX >= 2U && pageX < 6U && pageZ >= 2U && pageZ < 6U;
        }

        [[nodiscard]] std::optional<StitchEdge> pageStitchEdge(const std::uint32_t level,
                                                                 const std::uint32_t pageX,
                                                                 const std::uint32_t pageZ) {
            if (level == 0U) return std::nullopt;
            // Exactly one page edge can touch the central 4x4 hole. Corner
            // pages touch it only diagonally and need no index stitching.
            if (pageX == 1U && pageZ >= 2U && pageZ < 6U) return StitchEdge::Right;
            if (pageX == 6U && pageZ >= 2U && pageZ < 6U) return StitchEdge::Left;
            if (pageZ == 1U && pageX >= 2U && pageX < 6U) return StitchEdge::Top;
            if (pageZ == 6U && pageX >= 2U && pageX < 6U) return StitchEdge::Bottom;
            return std::nullopt;
        }

        void addOceanPage(Mesh &mesh, const std::uint32_t level, const std::uint32_t pageX,
                          const std::uint32_t pageZ) {
            const float outer = ClipmapExtents[level];
            const float cell = (2.0F * outer) / static_cast<float>(ClipmapResolution);
            const float pageSize = cell * static_cast<float>(WaterPageCells);
            const float minX = -outer + pageSize * static_cast<float>(pageX);
            const float minZ = -outer + pageSize * static_cast<float>(pageZ);
            const std::uint32_t firstVertex = static_cast<std::uint32_t>(mesh.vertices.size());
            const std::uint32_t firstIndex = static_cast<std::uint32_t>(mesh.indices.size());
            constexpr std::uint32_t PageVertexAxis = WaterPageCells + 1U;
            const auto vertexIndex = [firstVertex](const std::uint32_t x, const std::uint32_t z) {
                return firstVertex + z * PageVertexAxis + x;
            };

            // One page owns a compact 9x9 grid rather than four independent
            // vertices for every quad. Positions remain in the old local
            // clipmap coordinate system, so world-space phase is unchanged.
            for (std::uint32_t z = 0; z <= WaterPageCells; ++z)
                for (std::uint32_t x = 0; x <= WaterPageCells; ++x) {
                    const Vec3 position{minX + cell * static_cast<float>(x), 0.0F,
                                        minZ + cell * static_cast<float>(z)};
                    WaterSystem::addWaterVertex(mesh, position,
                        {static_cast<float>(x) / WaterPageCells, static_cast<float>(z) / WaterPageCells},
                        samplingCellSize(position, outer, cell, level));
                }

            const std::optional<StitchEdge> stitchedEdge = pageStitchEdge(level, pageX, pageZ);
            std::array<std::uint32_t, WaterPageCells> stitchMidpoints{};
            if (stitchedEdge.has_value()) {
                // The coarser edge has eight segments while its finer neighbour
                // has sixteen. Add one midpoint to each coarse segment, exactly
                // matching the old per-cell stitching without duplicating its
                // four corner vertices.
                for (std::uint32_t segment = 0; segment < WaterPageCells; ++segment) {
                    Vec3 a{}, b{};
                    switch (*stitchedEdge) {
                        case StitchEdge::Bottom:
                            a = {minX + cell * segment, 0.0F, minZ};
                            b = {minX + cell * (segment + 1U), 0.0F, minZ};
                            break;
                        case StitchEdge::Right:
                            a = {minX + pageSize, 0.0F, minZ + cell * segment};
                            b = {minX + pageSize, 0.0F, minZ + cell * (segment + 1U)};
                            break;
                        case StitchEdge::Top:
                            a = {minX + cell * segment, 0.0F, minZ + pageSize};
                            b = {minX + cell * (segment + 1U), 0.0F, minZ + pageSize};
                            break;
                        case StitchEdge::Left:
                            a = {minX, 0.0F, minZ + cell * segment};
                            b = {minX, 0.0F, minZ + cell * (segment + 1U)};
                            break;
                    }
                    stitchMidpoints[segment] = static_cast<std::uint32_t>(mesh.vertices.size());
                    const Vec3 midpoint = (a + b) * 0.5F;
                    WaterSystem::addWaterVertex(mesh, midpoint, {0.5F, 0.5F},
                        samplingCellSize(midpoint, outer, cell, level));
                }
            }

            for (std::uint32_t z = 0; z < WaterPageCells; ++z)
                for (std::uint32_t x = 0; x < WaterPageCells; ++x) {
                    const std::uint32_t a = vertexIndex(x, z);
                    const std::uint32_t b = vertexIndex(x + 1U, z);
                    const std::uint32_t c = vertexIndex(x + 1U, z + 1U);
                    const std::uint32_t d = vertexIndex(x, z + 1U);
                    const bool onStitchedEdge = stitchedEdge.has_value() &&
                        ((*stitchedEdge == StitchEdge::Bottom && z == 0U) ||
                         (*stitchedEdge == StitchEdge::Right && x + 1U == WaterPageCells) ||
                         (*stitchedEdge == StitchEdge::Top && z + 1U == WaterPageCells) ||
                         (*stitchedEdge == StitchEdge::Left && x == 0U));
                    if (onStitchedEdge) {
                        const std::uint32_t segment = *stitchedEdge == StitchEdge::Bottom ||
                                                      *stitchedEdge == StitchEdge::Top ? x : z;
                        addStitchedCellIndices(mesh, a, b, c, d, stitchMidpoints[segment], *stitchedEdge);
                    } else {
                        mesh.indices.insert(mesh.indices.end(), {a, b, c, a, c, d});
                    }
                }
            // This survives the geometry-heap upload as a logical page range.
            // The first GPU page culling pass consumes these ranges directly
            // instead of recreating or scanning the clipmap mesh on the CPU.
            mesh.drawRanges.push_back({
                .firstIndex = firstIndex,
                .indexCount = static_cast<std::uint32_t>(mesh.indices.size()) - firstIndex,
                // Gerstner displacement is added as a conservative material
                // bound by the page-culling extraction step. Keeping the base
                // footprint here means the mesh remains independent of a
                // particular water material or time sample.
                .localBounds = {.min = {minX, 0.0F, minZ},
                                .max = {minX + pageSize, 0.0F, minZ + pageSize}},
            });
        }
    } // namespace

    void WaterSystem::addWaterVertex(Mesh &mesh, const Vec3 &position, const Vec2 &uv, const float cellSize) {
        mesh.vertices.push_back({
            .position = position, .color = {1.0F, 1.0F, 1.0F}, .texCoord = uv,
            .normal = {0.0F, 1.0F, 0.0F}, .tangent = {1.0F, 0.0F, 0.0F, 1.0F},
            .texCoord1 = {cellSize, 0.0F}
        });
    }

    Mesh WaterSystem::buildOceanClipmap() {
        Mesh mesh;
        for (std::uint32_t level = 0; level < std::size(ClipmapExtents); ++level) {
            for (std::uint32_t pageZ = 0; pageZ < WaterPagesPerAxis; ++pageZ)
                for (std::uint32_t pageX = 0; pageX < WaterPagesPerAxis; ++pageX) {
                    if (pageIsInactive(level, pageX, pageZ)) continue;
                    addOceanPage(mesh, level, pageX, pageZ);
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
                constexpr float finestCell = (2.0F * ClipmapExtents[0]) / ClipmapResolution;
                const float snappedX = std::floor(cameraPosition.x() / finestCell) * finestCell;
                const float snappedZ = std::floor(cameraPosition.z() / finestCell) * finestCell;
                const TransformComponent &transform = registry.get<TransformComponent>(entity);
                if (transform.position.x() == snappedX && transform.position.z() == snappedZ) return;
                registry.modify<TransformComponent>(entity, [&](TransformComponent &transform) {
                    transform.position.setX(snappedX);
                    transform.position.setZ(snappedZ);
                });
            });
    }
} // namespace Engine
