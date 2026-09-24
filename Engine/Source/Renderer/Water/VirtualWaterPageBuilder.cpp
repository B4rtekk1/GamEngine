#include "Engine/Renderer/Water/VirtualWaterPageBuilder.h"

#include "Engine/Renderer/Water/WaterSystem.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>

namespace Engine::Water {
namespace {
[[nodiscard]] float smoothStep(const float minimum, const float maximum, const float value) noexcept {
    const float t = std::clamp((value - minimum) / (maximum - minimum), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

struct SpectrumCacheKey final {
    std::uint32_t revision{};
    std::uint32_t cellBits{};
    std::uint32_t waveCount{};
    [[nodiscard]] bool operator==(const SpectrumCacheKey&) const noexcept = default;
};
struct SpectrumCacheKeyHash final {
    [[nodiscard]] std::size_t operator()(const SpectrumCacheKey& key) const noexcept {
        std::uint64_t x = (static_cast<std::uint64_t>(key.revision) << 32U) | key.cellBits;
        x ^= static_cast<std::uint64_t>(key.waveCount) * 0x9e3779b97f4a7c15ULL;
        x ^= x >> 33U;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33U;
        return static_cast<std::size_t>(x);
    }
};

[[nodiscard]] bool pointInPolygon(const std::span<const Vec3> polygon,
                                  const float x,
                                  const float z) noexcept {
    if (polygon.size() < 3U) return false;
    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1U; i < polygon.size(); j = i++) {
        const Vec3& a = polygon[i];
        const Vec3& b = polygon[j];
        const float dz = b.z() - a.z();
        const bool crosses = ((a.z() > z) != (b.z() > z)) &&
            (x < (b.x() - a.x()) * (z - a.z()) /
                 (std::abs(dz) > 1.0e-7F ? dz : (dz >= 0.0F ? 1.0e-7F : -1.0e-7F)) + a.x());
        if (crosses) inside = !inside;
    }
    return inside;
}

void applySpectrumMetadata(GPUVirtualWaterPage& page, const SpectrumLODEntry& entry) noexcept {
    page.waveCandidateMask = entry.waveCandidateMask;
    page.verticalBound = entry.verticalBound;
    page.horizontalBound = entry.horizontalBound;
    page.unresolvedSlopeVariance = entry.unresolvedSlopeVariance;
    page.unresolvedSlopeXX = entry.unresolvedSlopeXX;
    page.unresolvedSlopeXZ = entry.unresolvedSlopeXZ;
    page.unresolvedSlopeZZ = entry.unresolvedSlopeZZ;
    page.normalDetailImportance = entry.normalDetailImportance;
}

void linkNeighbours(std::vector<GPUVirtualWaterPage>& pages, const std::uint32_t pageBaseIndex) {
    const auto findNeighbour = [&](const GPUVirtualWaterPage& source,
                                   const float sampleX,
                                   const float sampleZ) -> std::uint32_t {
        std::uint32_t best = InvalidPhysicalPage;
        float bestCell = std::numeric_limits<float>::max();
        for (std::uint32_t i = 0; i < pages.size(); ++i) {
            const GPUVirtualWaterPage& candidate = pages[i];
            if (&candidate == &source ||
                candidate.coverageClass == static_cast<std::uint32_t>(WaterCoverageClass::Dry)) continue;
            const float epsilon = std::max(candidate.cellSize * 0.02F, 1.0e-3F);
            if (sampleX < candidate.minX - epsilon || sampleX > candidate.maxX + epsilon ||
                sampleZ < candidate.minZ - epsilon || sampleZ > candidate.maxZ + epsilon) continue;
            if (candidate.cellSize < bestCell) {
                bestCell = candidate.cellSize;
                best = pageBaseIndex + i;
            }
        }
        return best;
    };

    for (GPUVirtualWaterPage& page : pages) {
        if ((page.flags & PageSpline) != 0U) continue;
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
} // namespace

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

SpectrumLODEntry spectrumLOD(const std::span<const WaterGerstnerWave> waves,
                             const std::uint32_t waveCountValue,
                             const std::uint32_t spectrumRevision,
                             const float cellSize) {
    static std::unordered_map<SpectrumCacheKey, SpectrumLODEntry, SpectrumCacheKeyHash> cache;
    const std::uint32_t count = std::min<std::uint32_t>(waveCountValue,
        static_cast<std::uint32_t>(waves.size()));
    const SpectrumCacheKey key{spectrumRevision, std::bit_cast<std::uint32_t>(cellSize), count};
    if (const auto found = cache.find(key); found != cache.end()) return found->second;

    SpectrumLODEntry entry{};
    for (std::uint32_t waveIndex = 0; waveIndex < count; ++waveIndex) {
        const WaterGerstnerWave& wave = waves[waveIndex];
        const float weight = spectralWeight(wave.wavelength, cellSize);
        if (weight > 1.0e-6F) entry.waveCandidateMask |= 1U << waveIndex;
        entry.verticalBound += std::abs(wave.amplitude * weight);
        entry.horizontalBound += std::abs(wave.steepness * wave.amplitude * weight);
        const float k = 6.28318530718F / std::max(wave.wavelength, 1.0e-3F);
        const float slope = wave.amplitude * k;
        const float variance = 0.5F * (1.0F - weight * weight) * slope * slope;
        entry.unresolvedSlopeVariance += variance;
        Vec2 direction = wave.direction;
        if (direction.length() < 1.0e-5F) direction = {1.0F, 0.0F};
        else direction = direction.normalized();
        entry.unresolvedSlopeXX += variance * direction.x() * direction.x();
        entry.unresolvedSlopeXZ += variance * direction.x() * direction.y();
        entry.unresolvedSlopeZZ += variance * direction.y() * direction.y();
        entry.normalDetailImportance += variance;
    }
    if (cache.size() > 1024U) cache.clear();
    cache.emplace(key, entry);
    return entry;
}

std::vector<GPUVirtualWaterPage> buildOceanPageDomain(const OceanDomainConfig& config) {
    std::vector<GPUVirtualWaterPage> pages;
    pages.reserve(ActivePagesPerBody);
    const std::uint32_t waveCount = std::min<std::uint32_t>(config.waveCount,
        static_cast<std::uint32_t>(config.waves.size()));

    const std::uint32_t levelCount = std::clamp(config.geometryLevelCount, 1U, ClipLevels);
    for (std::uint32_t level = 0; level < levelCount; ++level) {
        const float outer = config.extents[level];
        const float cell = (2.0F * outer) / static_cast<float>(ClipmapResolution);
        const float pageSize = cell * static_cast<float>(PageCells);
        const SpectrumLODEntry spectrum = spectrumLOD(config.waves, waveCount, config.spectrumRevision, cell);
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
                page.flags = PageActive | PageCameraRelative;
                page.instanceIndex = config.instanceIndex;
                page.bodyIndex = config.bodyIndex;
                page.bodyIdIndex = config.bodyId.index;
                page.bodyIdGeneration = config.bodyId.generation;
                page.spectrumRevision = config.spectrumRevision;
                page.localPageX = static_cast<std::int32_t>(x) - static_cast<std::int32_t>(PagesPerAxis / 2U);
                page.localPageZ = static_cast<std::int32_t>(z) - static_cast<std::int32_t>(PagesPerAxis / 2U);
                page.coverageClass = static_cast<std::uint32_t>(WaterCoverageClass::Wet);
                page.executionFlags = config.executionFlags;
                // The persistent ocean state domain uses the finest world grid
                // independently of geometric LOD. Coarser geometry pages can
                // overlap many state cells, so binding a single physical state
                // page to them would clamp/alias wakes. Only the level-0 pages
                // own state mappings; their world identity is reconnected after
                // clipmap recentering by WaterStatePageKey.
                if(level!=0U) page.executionFlags &= ~ExecutionSparseState;
                const float stateWorldSize = (2.0F * config.extents[0]) / static_cast<float>(PagesPerAxis);
                page.stateWorldSizeClass = std::bit_cast<std::uint32_t>(stateWorldSize);
                page.drawBin = config.bodyIndex * StitchVariantCount + page.stitchMask;
                if (level + 1U < levelCount) {
                    if (x == 0U) page.spectralEdgeMask |= StitchLeft;
                    if (x + 1U == PagesPerAxis) page.spectralEdgeMask |= StitchRight;
                    if (z == 0U) page.spectralEdgeMask |= StitchBottom;
                    if (z + 1U == PagesPerAxis) page.spectralEdgeMask |= StitchTop;
                }
                applySpectrumMetadata(page, spectrum);
                pages.push_back(page);
            }
        }
    }
    linkNeighbours(pages, config.pageBaseIndex);
    return pages;
}

std::vector<GPUVirtualWaterPage> buildFiniteWaterPageDomain(const FiniteWaterDomainConfig& config) {
    std::vector<GPUVirtualWaterPage> pages;
    if (config.boundary.size() < 3U) return pages;
    float minX = std::numeric_limits<float>::max(), minZ = minX;
    float maxX = std::numeric_limits<float>::lowest(), maxZ = maxX;
    float height = 0.0F;
    for (const Vec3& point : config.boundary) {
        minX = std::min(minX, point.x()); minZ = std::min(minZ, point.z());
        maxX = std::max(maxX, point.x()); maxZ = std::max(maxZ, point.z()); height += point.y();
    }
    height /= static_cast<float>(config.boundary.size());
    const float spanX = std::max(maxX - minX, 1.0F);
    const float spanZ = std::max(maxZ - minZ, 1.0F);
    float pageSize = std::max(config.targetPageWorldSize, 1.0F);
    std::uint32_t countX = static_cast<std::uint32_t>(std::ceil(spanX / pageSize));
    std::uint32_t countZ = static_cast<std::uint32_t>(std::ceil(spanZ / pageSize));
    const float scale = std::sqrt(static_cast<float>(std::max(1U, countX * countZ)) /
                                  static_cast<float>(ActivePagesPerBody));
    if (scale > 1.0F) {
        pageSize *= scale;
        countX = static_cast<std::uint32_t>(std::ceil(spanX / pageSize));
        countZ = static_cast<std::uint32_t>(std::ceil(spanZ / pageSize));
    }
    const float cellSize = pageSize / static_cast<float>(PageCells);
    const SpectrumLODEntry spectrum = spectrumLOD(config.waves, config.waveCount,
                                                   config.spectrumRevision, cellSize);
    pages.reserve(std::min<std::uint32_t>(countX * countZ, ActivePagesPerBody));
    for (std::uint32_t z = 0; z < countZ && pages.size() < ActivePagesPerBody; ++z) {
        for (std::uint32_t x = 0; x < countX && pages.size() < ActivePagesPerBody; ++x) {
            GPUVirtualWaterPage page{};
            page.originX = minX + static_cast<float>(x) * pageSize;
            page.originZ = minZ + static_cast<float>(z) * pageSize;
            page.minX = page.originX; page.minZ = page.originZ;
            page.maxX = std::min(page.originX + pageSize, maxX);
            page.maxZ = std::min(page.originZ + pageSize, maxZ);
            page.baseHeight = height; page.cellSize = cellSize;
            page.level = 0U; page.stitchMask = 0U; page.flags = PageActive;
            page.instanceIndex = config.instanceIndex; page.bodyIndex = config.bodyIndex;
            page.bodyIdIndex = config.bodyId.index; page.bodyIdGeneration = config.bodyId.generation;
            page.spectrumRevision = config.spectrumRevision;
            page.localPageX = static_cast<std::int32_t>(x);
            page.localPageZ = static_cast<std::int32_t>(z);
            page.executionFlags = config.executionFlags;
            page.stateWorldSizeClass = std::bit_cast<std::uint32_t>(pageSize);
            page.drawBin = config.bodyIndex * StitchVariantCount;
            applySpectrumMetadata(page, spectrum);

            std::uint64_t mask = 0U;
            for (std::uint32_t cz = 0; cz < PageCells; ++cz) {
                for (std::uint32_t cx = 0; cx < PageCells; ++cx) {
                    const float px = page.originX + (static_cast<float>(cx) + 0.5F) * cellSize;
                    const float pz = page.originZ + (static_cast<float>(cz) + 0.5F) * cellSize;
                    if (pointInPolygon(config.boundary, px, pz)) mask |= 1ULL << (cz * PageCells + cx);
                }
            }
            if (mask == 0U) continue;
            page.coverageClass = mask == ~0ULL
                ? static_cast<std::uint32_t>(WaterCoverageClass::Wet)
                : static_cast<std::uint32_t>(WaterCoverageClass::Partial);
            if (mask != ~0ULL) page.flags |= PageShoreline;
            page.cellCoverageMaskLow = static_cast<std::uint32_t>(mask);
            page.cellCoverageMaskHigh = static_cast<std::uint32_t>(mask >> 32U);
            pages.push_back(page);
        }
    }
    linkNeighbours(pages, config.pageBaseIndex);
    return pages;
}

std::vector<GPUVirtualWaterPage> buildSplinePageDomain(const SplineDomainConfig& config) {
    std::vector<GPUVirtualWaterPage> pages;
    if (config.points.size() < 2U) return pages;
    pages.reserve(std::min<std::size_t>(config.points.size() - 1U, ActivePagesPerBody));
    for (std::size_t segment = 0; segment + 1U < config.points.size() && pages.size() < ActivePagesPerBody; ++segment) {
        const RiverSplinePoint& a = config.points[segment];
        const RiverSplinePoint& b = config.points[segment + 1U];
        const float dx = b.position.x() - a.position.x();
        const float dz = b.position.z() - a.position.z();
        const float length = std::sqrt(dx * dx + dz * dz);
        if (length < 1.0e-4F) continue;
        const float halfA = std::max(a.width * 0.5F, 0.05F);
        const float halfB = std::max(b.width * 0.5F, 0.05F);
        const float nx = -dz / length, nz = dx / length;
        GPUVirtualWaterPage page{};
        page.flags = PageActive | PageSpline;
        page.instanceIndex = config.instanceIndex;
        page.bodyIndex = config.bodyIndex;
        page.bodyIdIndex = config.bodyId.index;
        page.bodyIdGeneration = config.bodyId.generation;
        page.spectrumRevision = config.spectrumRevision;
        page.executionFlags = config.executionFlags;
        page.stateWorldSizeClass = std::bit_cast<std::uint32_t>(std::max(length, 1.0F));
        page.localPageX = static_cast<std::int32_t>(segment);
        page.localPageZ = 0;
        page.level = 0U;
        page.drawBin = config.bodyIndex * StitchVariantCount;
        page.splineStartX = a.position.x(); page.splineStartZ = a.position.z();
        page.splineEndX = b.position.x(); page.splineEndZ = b.position.z();
        page.splineHalfWidth0 = halfA; page.splineHalfWidth1 = halfB;
        page.originX = std::min({a.position.x() + nx * halfA, a.position.x() - nx * halfA,
                                 b.position.x() + nx * halfB, b.position.x() - nx * halfB});
        page.originZ = std::min({a.position.z() + nz * halfA, a.position.z() - nz * halfA,
                                 b.position.z() + nz * halfB, b.position.z() - nz * halfB});
        page.minX = page.originX; page.minZ = page.originZ;
        page.maxX = std::max({a.position.x() + nx * halfA, a.position.x() - nx * halfA,
                              b.position.x() + nx * halfB, b.position.x() - nx * halfB});
        page.maxZ = std::max({a.position.z() + nz * halfA, a.position.z() - nz * halfA,
                              b.position.z() + nz * halfB, b.position.z() - nz * halfB});
        page.baseHeight = a.position.y();
        page.reservedFloat = b.position.y();
        page.cellSize = std::max(length, std::max(halfA, halfB) * 2.0F) / static_cast<float>(PageCells);
        page.flowX = dx / length; page.flowZ = dz / length;
        page.flowSpeed = std::max(0.0F, 0.5F * (a.flowSpeed + b.flowSpeed));
        page.coverageClass = static_cast<std::uint32_t>(WaterCoverageClass::Wet);
        applySpectrumMetadata(page, spectrumLOD(config.waves, config.waveCount,
                                                 config.spectrumRevision, page.cellSize));
        pages.push_back(page);
    }
    for (std::uint32_t i = 0; i < pages.size(); ++i) {
        if (i > 0U) pages[i].neighborLeft0 = pages[i].neighborLeft1 = config.pageBaseIndex + i - 1U;
        if (i + 1U < pages.size()) pages[i].neighborRight0 = pages[i].neighborRight1 = config.pageBaseIndex + i + 1U;
    }
    return pages;
}

Mesh buildReusableOceanPageMesh() {
    Mesh mesh;
    constexpr std::uint32_t axis = PageVertexAxis;
    const auto gridIndex = [](const std::uint32_t x, const std::uint32_t z) { return z * axis + x; };

    for (std::uint32_t z = 0; z <= PageCells; ++z) {
        for (std::uint32_t x = 0; x <= PageCells; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(PageCells);
            const float v = static_cast<float>(z) / static_cast<float>(PageCells);
            WaterSystem::addWaterVertex(mesh, {u, 0.0F, v}, {u, v}, 0.0F);
        }
    }

    std::array<std::uint32_t, PageCells> bottom{}, right{}, top{}, left{};
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
    mesh.recalculateLocalBounds();
    return mesh;
}
} // namespace Engine::Water
