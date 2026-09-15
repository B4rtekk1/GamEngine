#include "Engine/Renderer/Water/VirtualWaterPageBuilder.h"

#include "Engine/Renderer/Water/WaterSystem.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace Engine::Water {
    namespace {
        [[nodiscard]] float smoothStep(const float minimum, const float maximum, const float value) noexcept {
            const float t = std::clamp((value - minimum) / (maximum - minimum), 0.0F, 1.0F);
            return t * t * (3.0F - 2.0F * t);
        }

        enum class StitchEdge : std::uint8_t { Bottom, Right, Top, Left };

        void addStitchedCellIndices(Mesh& mesh,
                                    const std::uint32_t a,
                                    const std::uint32_t b,
                                    const std::uint32_t c,
                                    const std::uint32_t d,
                                    const std::uint32_t midpoint,
                                    const StitchEdge edge) {
            switch (edge) {
                case StitchEdge::Bottom:
                    mesh.indices.insert(mesh.indices.end(), {a, midpoint, d, midpoint, c, d, midpoint, b, c});
                    break;
                case StitchEdge::Right:
                    mesh.indices.insert(mesh.indices.end(), {b, midpoint, a, midpoint, d, a, midpoint, c, d});
                    break;
                case StitchEdge::Top:
                    mesh.indices.insert(mesh.indices.end(), {d, midpoint, a, midpoint, b, a, midpoint, c, b});
                    break;
                case StitchEdge::Left:
                    mesh.indices.insert(mesh.indices.end(), {a, midpoint, b, midpoint, c, b, midpoint, d, c});
                    break;
            }
        }
    }

    float spectralWeight(const float wavelength, const float cellSize) noexcept {
        if (cellSize <= 0.0F) return 1.0F;
        return smoothStep(2.0F, 6.0F, wavelength / cellSize);
    }

    bool pageIsInactive(const std::uint32_t level,
                        const std::uint32_t x,
                        const std::uint32_t z) noexcept {
        return level != 0U && x >= 2U && x < 6U && z >= 2U && z < 6U;
    }

    std::uint32_t pageStitchMask(const std::uint32_t level,
                                 const std::uint32_t x,
                                 const std::uint32_t z) noexcept {
        if (level == 0U) return 0U;
        if (x == 6U && z >= 2U && z < 6U) return StitchLeft;
        if (x == 1U && z >= 2U && z < 6U) return StitchRight;
        if (z == 6U && x >= 2U && x < 6U) return StitchBottom;
        if (z == 1U && x >= 2U && x < 6U) return StitchTop;
        return 0U;
    }

    std::vector<GPUVirtualWaterPage> buildOceanPageDomain(const OceanDomainConfig& config) {
        std::vector<GPUVirtualWaterPage> pages;
        pages.reserve(ActivePagesPerBody);
        const std::uint32_t waveCount = std::min<std::uint32_t>(
            config.waveCount, static_cast<std::uint32_t>(config.waves.size()));

        const std::uint32_t levelCount = std::clamp(config.geometryLevelCount, 1U, ClipLevels);
        for (std::uint32_t level = 0; level < levelCount; ++level) {
            const float outer = config.extents[level];
            const float cell = (2.0F * outer) / static_cast<float>(ClipmapResolution);
            const float pageSize = cell * static_cast<float>(PageCells);
            for (std::uint32_t z = 0; z < PagesPerAxis; ++z) {
                for (std::uint32_t x = 0; x < PagesPerAxis; ++x) {
                    if (pageIsInactive(level, x, z)) continue;
                    GPUVirtualWaterPage page{};
                    page.originX = -outer + pageSize * static_cast<float>(x);
                    page.originZ = -outer + pageSize * static_cast<float>(z);
                    page.baseHeight = 0.0F;
                    page.cellSize = cell;
                    page.minX = page.originX;
                    page.minZ = page.originZ;
                    page.maxX = page.originX + pageSize;
                    page.maxZ = page.originZ + pageSize;
                    page.level = level;
                    page.stitchMask = pageStitchMask(level, x, z);
                    page.flags = PageActive;
                    page.instanceIndex = config.instanceIndex;
                    page.bodyIndex = config.bodyIndex;
                    page.drawBin = config.bodyIndex * StitchVariantCount + page.stitchMask;
                    if (level + 1U < levelCount) {
                        if (x == 0U) page.spectralEdgeMask |= StitchLeft;
                        if (x + 1U == PagesPerAxis) page.spectralEdgeMask |= StitchRight;
                        if (z == 0U) page.spectralEdgeMask |= StitchBottom;
                        if (z + 1U == PagesPerAxis) page.spectralEdgeMask |= StitchTop;
                    }

                    for (std::uint32_t waveIndex = 0; waveIndex < waveCount; ++waveIndex) {
                        const WaterGerstnerWave& wave = config.waves[waveIndex];
                        const float weight = spectralWeight(wave.wavelength, cell);
                        if (weight > 1.0e-6F) page.waveCandidateMask |= 1U << waveIndex;
                        page.verticalBound += std::abs(wave.amplitude * weight);
                        page.horizontalBound += std::abs(wave.steepness * wave.amplitude * weight);
                        const float k = 6.28318530718F / std::max(wave.wavelength, 1.0e-3F);
                        const float slope = wave.amplitude * k;
                        page.unresolvedSlopeVariance +=
                            0.5F * (1.0F - weight * weight) * slope * slope;
                    }
                    // verticalBound describes analytic Gerstner displacement only.
                    // Persistent state height is stored in world metres (it is not scaled
                    // by the water-body transform), so water_page_cull adds that envelope
                    // after transforming each analytic corner to world space.
                    pages.push_back(page);
                }
            }
        }

        const auto findNeighbour = [&](const GPUVirtualWaterPage& source, const float sampleX,
                                       const float sampleZ) -> std::uint32_t {
            std::uint32_t best = InvalidPhysicalPage;
            float bestCell = std::numeric_limits<float>::max();
            for (std::uint32_t i = 0; i < pages.size(); ++i) {
                const GPUVirtualWaterPage& candidate = pages[i];
                if (&candidate == &source) continue;
                const float epsilon = std::max(candidate.cellSize * 0.02F, 1.0e-3F);
                if (sampleX < candidate.minX - epsilon || sampleX > candidate.maxX + epsilon ||
                    sampleZ < candidate.minZ - epsilon || sampleZ > candidate.maxZ + epsilon) continue;
                // At a 2:1 boundary prefer the finer page. Two samples along
                // the edge select the two fine neighbours of one coarse page.
                if (candidate.cellSize < bestCell) {
                    bestCell = candidate.cellSize;
                    best = config.pageBaseIndex + i;
                }
            }
            return best;
        };

        for (GPUVirtualWaterPage& page : pages) {
            const float spanX = page.maxX - page.minX;
            const float spanZ = page.maxZ - page.minZ;
            const float edgeEpsilon = std::max(page.cellSize * 0.05F, 1.0e-3F);
            page.neighborLeft0 = findNeighbour(page, page.minX - edgeEpsilon, page.minZ + spanZ * 0.25F);
            page.neighborLeft1 = findNeighbour(page, page.minX - edgeEpsilon, page.minZ + spanZ * 0.75F);
            page.neighborRight0 = findNeighbour(page, page.maxX + edgeEpsilon, page.minZ + spanZ * 0.25F);
            page.neighborRight1 = findNeighbour(page, page.maxX + edgeEpsilon, page.minZ + spanZ * 0.75F);
            page.neighborBottom0 = findNeighbour(page, page.minX + spanX * 0.25F, page.minZ - edgeEpsilon);
            page.neighborBottom1 = findNeighbour(page, page.minX + spanX * 0.75F, page.minZ - edgeEpsilon);
            page.neighborTop0 = findNeighbour(page, page.minX + spanX * 0.25F, page.maxZ + edgeEpsilon);
            page.neighborTop1 = findNeighbour(page, page.minX + spanX * 0.75F, page.maxZ + edgeEpsilon);
        }
        return pages;
    }

    Mesh buildReusableOceanPageMesh() {
        Mesh mesh;
        constexpr std::uint32_t axis = PageVertexAxis;
        const auto gridIndex = [](const std::uint32_t x, const std::uint32_t z) {
            return z * axis + x;
        };

        // 9x9 shared grid.
        for (std::uint32_t z = 0; z <= PageCells; ++z) {
            for (std::uint32_t x = 0; x <= PageCells; ++x) {
                const float u = static_cast<float>(x) / static_cast<float>(PageCells);
                const float v = static_cast<float>(z) / static_cast<float>(PageCells);
                WaterSystem::addWaterVertex(mesh, {u, 0.0F, v}, {u, v}, 0.0F);
            }
        }

        std::array<std::uint32_t, PageCells> bottom{};
        std::array<std::uint32_t, PageCells> right{};
        std::array<std::uint32_t, PageCells> top{};
        std::array<std::uint32_t, PageCells> left{};
        const auto addMidpoint = [&](std::array<std::uint32_t, PageCells>& target,
                                     const std::uint32_t segment,
                                     const float x,
                                     const float z) {
            target[segment] = static_cast<std::uint32_t>(mesh.vertices.size());
            WaterSystem::addWaterVertex(mesh, {x, 0.0F, z}, {x, z}, 0.0F);
        };
        for (std::uint32_t segment = 0; segment < PageCells; ++segment) {
            const float t = (static_cast<float>(segment) + 0.5F) / static_cast<float>(PageCells);
            addMidpoint(bottom, segment, t, 0.0F);
            addMidpoint(right, segment, 1.0F, t);
            addMidpoint(top, segment, t, 1.0F);
            addMidpoint(left, segment, 0.0F, t);
        }

        for (std::uint32_t mask = 0; mask < StitchVariantCount; ++mask) {
            const std::uint32_t firstIndex = static_cast<std::uint32_t>(mesh.indices.size());
            for (std::uint32_t z = 0; z < PageCells; ++z) {
                for (std::uint32_t x = 0; x < PageCells; ++x) {
                    const std::uint32_t a = gridIndex(x, z);
                    const std::uint32_t b = gridIndex(x + 1U, z);
                    const std::uint32_t c = gridIndex(x + 1U, z + 1U);
                    const std::uint32_t d = gridIndex(x, z + 1U);
                    // Current clipmap topology generates at most one stitched edge for a page.
                    // The priority keeps all 16 variants valid and deterministic for future LOD policies.
                    if ((mask & StitchBottom) != 0U && z == 0U) {
                        addStitchedCellIndices(mesh, a, b, c, d, bottom[x], StitchEdge::Bottom);
                    } else if ((mask & StitchRight) != 0U && x + 1U == PageCells) {
                        addStitchedCellIndices(mesh, a, b, c, d, right[z], StitchEdge::Right);
                    } else if ((mask & StitchTop) != 0U && z + 1U == PageCells) {
                        addStitchedCellIndices(mesh, a, b, c, d, top[x], StitchEdge::Top);
                    } else if ((mask & StitchLeft) != 0U && x == 0U) {
                        addStitchedCellIndices(mesh, a, b, c, d, left[z], StitchEdge::Left);
                    } else {
                        mesh.indices.insert(mesh.indices.end(), {a, b, c, a, c, d});
                    }
                }
            }
            mesh.drawRanges.push_back({
                .firstIndex = firstIndex,
                .indexCount = static_cast<std::uint32_t>(mesh.indices.size()) - firstIndex,
                .localBounds = {.min = {0.0F, 0.0F, 0.0F}, .max = {1.0F, 0.0F, 1.0F}},
            });
        }
        return mesh;
    }
} // namespace Engine::Water
