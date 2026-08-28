#include "Engine/ECS/Components/TerrainComponent.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <stdexcept>

namespace Engine {
namespace {

[[nodiscard]] std::size_t checkedSampleCount(const std::uint32_t resolution) {
    if (resolution < TerrainComponent::MinimumResolution ||
        resolution > TerrainComponent::MaximumResolution) {
        throw std::invalid_argument("Terrain resolution must be between 2 and 513");
    }
    return static_cast<std::size_t>(resolution) * resolution;
}

[[nodiscard]] float brushFalloff(const float normalizedDistance,
                                 const TerrainBrushFalloff falloff) noexcept {
    const float value = std::clamp(1.0F - normalizedDistance, 0.0F, 1.0F);
    if (falloff == TerrainBrushFalloff::Linear) return value;
    if (falloff == TerrainBrushFalloff::Sharp) return value * value;
    return value * value * (3.0F - 2.0F * value);
}

} // namespace

TerrainComponent::TerrainComponent()
    : heights(checkedSampleCount(resolution), 0.0F) {}

TerrainComponent::TerrainComponent(const std::uint32_t requestedResolution,
                                   const float requestedWidth,
                                   const float requestedDepth,
                                   const float requestedMinimumHeight,
                                   const float requestedMaximumHeight)
    : resolution(requestedResolution), width(requestedWidth), depth(requestedDepth),
      minimumHeight(requestedMinimumHeight), maximumHeight(requestedMaximumHeight),
      heights(checkedSampleCount(requestedResolution), 0.0F) {
    if (!valid()) throw std::invalid_argument("Terrain dimensions or height range are invalid");
}

bool TerrainComponent::valid() const noexcept {
    return resolution >= MinimumResolution && resolution <= MaximumResolution &&
           width > 0.0F && depth > 0.0F && minimumHeight < maximumHeight &&
           std::isfinite(width) && std::isfinite(depth) &&
           std::isfinite(minimumHeight) && std::isfinite(maximumHeight) &&
           heights.size() == sampleCount() &&
           std::ranges::all_of(heights, [this](const float value) {
               return std::isfinite(value) && value >= minimumHeight && value <= maximumHeight;
           });
}

std::size_t TerrainComponent::sampleCount() const noexcept {
    return static_cast<std::size_t>(resolution) * resolution;
}

float TerrainComponent::height(const std::uint32_t x, const std::uint32_t z) const {
    if (x >= resolution || z >= resolution) throw std::out_of_range("Terrain sample is outside the heightmap");
    return heights[static_cast<std::size_t>(z) * resolution + x];
}

float TerrainComponent::sampleHeight(const float localX, const float localZ) const noexcept {
    if (!valid()) return 0.0F;
    const float gridX = std::clamp((localX / width + 0.5F) * static_cast<float>(resolution - 1),
                                   0.0F, static_cast<float>(resolution - 1));
    const float gridZ = std::clamp((localZ / depth + 0.5F) * static_cast<float>(resolution - 1),
                                   0.0F, static_cast<float>(resolution - 1));
    const auto x0 = static_cast<std::uint32_t>(std::floor(gridX));
    const auto z0 = static_cast<std::uint32_t>(std::floor(gridZ));
    const auto x1 = std::min(x0 + 1, resolution - 1);
    const auto z1 = std::min(z0 + 1, resolution - 1);
    const float tx = gridX - static_cast<float>(x0);
    const float tz = gridZ - static_cast<float>(z0);
    const float nearHeight = std::lerp(height(x0, z0), height(x1, z0), tx);
    const float farHeight = std::lerp(height(x0, z1), height(x1, z1), tx);
    return std::lerp(nearHeight, farHeight, tz);
}

Mesh TerrainComponent::createMesh(const std::uint32_t lodLevel) const {
    if (!valid()) throw std::logic_error("Cannot build an invalid terrain");

    Mesh mesh;
    const std::uint32_t cellCount = resolution - 1;
    const std::uint32_t maximumLod = static_cast<std::uint32_t>(
        std::floor(std::log2(static_cast<float>(cellCount))));
    const std::uint32_t step = 1U << std::min(lodLevel, maximumLod);
    std::vector<std::uint32_t> samples;
    for (std::uint32_t sample = 0; sample < cellCount; sample += step) samples.push_back(sample);
    if (samples.empty() || samples.back() != cellCount) samples.push_back(cellCount);
    const std::uint32_t meshResolution = static_cast<std::uint32_t>(samples.size());
    mesh.vertices.reserve(static_cast<std::size_t>(meshResolution) * meshResolution);
    mesh.indices.reserve(static_cast<std::size_t>(meshResolution - 1) * (meshResolution - 1) * 6);
    const float spacingX = width / static_cast<float>(cellCount);
    const float spacingZ = depth / static_cast<float>(cellCount);

    const auto normalAt = [&](const std::uint32_t x, const std::uint32_t z) {
        const std::uint32_t left = x == 0 ? x : x - 1;
        const std::uint32_t right = std::min(x + 1, resolution - 1);
        const std::uint32_t near = z == 0 ? z : z - 1;
        const std::uint32_t far = std::min(z + 1, resolution - 1);
        const float dxDistance = static_cast<float>(right - left) * spacingX;
        const float dzDistance = static_cast<float>(far - near) * spacingZ;
        const float slopeX = (height(right, z) - height(left, z)) / std::max(dxDistance, 1.0e-6F);
        const float slopeZ = (height(x, far) - height(x, near)) / std::max(dzDistance, 1.0e-6F);
        return Vec3{-slopeX, 1.0F, -slopeZ}.normalized();
    };

    for (const std::uint32_t sampleZ : samples) {
        for (const std::uint32_t sampleX : samples) {
            // A negative red channel marks procedural terrain colouring.  A
            // per-cell checker cannot be stored in shared corner vertices,
            // so the fragment shader reconstructs it from UV and cell count.
            const Vec3 color{-1.0F, static_cast<float>(cellCount), 0.0F};
            const float px = -width * 0.5F + static_cast<float>(sampleX) * spacingX;
            const float pz = -depth * 0.5F + static_cast<float>(sampleZ) * spacingZ;
            mesh.vertices.push_back(Vertex{
                .position = {px, height(sampleX, sampleZ), pz},
                .color = color,
                .texCoord = {static_cast<float>(sampleX) / cellCount,
                             static_cast<float>(sampleZ) / cellCount},
                .normal = normalAt(sampleX, sampleZ),
                .tangent = {1.0F, 0.0F, 0.0F, 1.0F},
            });
        }
    }
    for (std::uint32_t z = 0; z + 1 < meshResolution; ++z) {
        for (std::uint32_t x = 0; x + 1 < meshResolution; ++x) {
            const std::uint32_t first = z * meshResolution + x;
            mesh.indices.insert(mesh.indices.end(), {
                first, first + meshResolution + 1, first + 1,
                first + meshResolution + 1, first, first + meshResolution,
            });
        }
    }
    return mesh;
}

bool TerrainComponent::updateMeshRegion(Mesh& mesh, const TerrainRegion& region) const {
    if (!valid() || !region.valid || mesh.vertices.size() != sampleCount()) return false;
    const float spacingX = width / static_cast<float>(resolution - 1);
    const float spacingZ = depth / static_cast<float>(resolution - 1);
    const std::uint32_t minX = region.minimumX == 0 ? 0 : region.minimumX - 1;
    const std::uint32_t minZ = region.minimumZ == 0 ? 0 : region.minimumZ - 1;
    const std::uint32_t maxX = std::min(region.maximumX + 1, resolution - 1);
    const std::uint32_t maxZ = std::min(region.maximumZ + 1, resolution - 1);
    for (std::uint32_t z = minZ; z <= maxZ; ++z) {
        for (std::uint32_t x = minX; x <= maxX; ++x) {
            const std::uint32_t left = x == 0 ? x : x - 1;
            const std::uint32_t right = std::min(x + 1, resolution - 1);
            const std::uint32_t near = z == 0 ? z : z - 1;
            const std::uint32_t far = std::min(z + 1, resolution - 1);
            const float slopeX = (height(right, z) - height(left, z)) /
                std::max(static_cast<float>(right - left) * spacingX, 1.0e-6F);
            const float slopeZ = (height(x, far) - height(x, near)) /
                std::max(static_cast<float>(far - near) * spacingZ, 1.0e-6F);
            Vertex& vertex = mesh.vertices[static_cast<std::size_t>(z) * resolution + x];
            vertex.position.setY(height(x, z));
            vertex.normal = Vec3{-slopeX, 1.0F, -slopeZ}.normalized();
        }
    }
    return true;
}

bool TerrainComponent::sculpt(const float localX, const float localZ, const float radius,
                              const float amount, const TerrainSculptMode mode,
                              const float flattenHeight, const TerrainBrushFalloff falloff,
                              TerrainRegion* changedRegion) {
    if (!valid() || radius <= 0.0F || amount <= 0.0F || !std::isfinite(localX) ||
        !std::isfinite(localZ) || !std::isfinite(radius) || !std::isfinite(amount)) return false;

    const float spacingX = width / static_cast<float>(resolution - 1);
    const float spacingZ = depth / static_cast<float>(resolution - 1);
    const auto gridX = [this, spacingX](const float value) {
        return std::clamp(static_cast<int>(std::floor((value + width * 0.5F) / spacingX)),
                          0, static_cast<int>(resolution - 1));
    };
    const auto gridZ = [this, spacingZ](const float value) {
        return std::clamp(static_cast<int>(std::floor((value + depth * 0.5F) / spacingZ)),
                          0, static_cast<int>(resolution - 1));
    };
    const std::uint32_t minimumX = static_cast<std::uint32_t>(gridX(localX - radius));
    const std::uint32_t maximumX = static_cast<std::uint32_t>(
        std::min(gridX(localX + radius) + 1, static_cast<int>(resolution - 1)));
    const std::uint32_t minimumZ = static_cast<std::uint32_t>(gridZ(localZ - radius));
    const std::uint32_t maximumZ = static_cast<std::uint32_t>(
        std::min(gridZ(localZ + radius) + 1, static_cast<int>(resolution - 1)));
    const std::uint32_t smoothMinimumX = minimumX == 0 ? 0 : minimumX - 1;
    const std::uint32_t smoothMinimumZ = minimumZ == 0 ? 0 : minimumZ - 1;
    const std::uint32_t smoothMaximumX = std::min(maximumX + 1, resolution - 1);
    const std::uint32_t smoothMaximumZ = std::min(maximumZ + 1, resolution - 1);
    const std::uint32_t smoothWidth = smoothMaximumX - smoothMinimumX + 1;
    std::vector<float> smoothSource;
    if (mode == TerrainSculptMode::Smooth) {
        smoothSource.reserve(static_cast<std::size_t>(smoothWidth) *
                             (smoothMaximumZ - smoothMinimumZ + 1));
        for (std::uint32_t z = smoothMinimumZ; z <= smoothMaximumZ; ++z) {
            const auto begin = heights.begin() + static_cast<std::ptrdiff_t>(z) * resolution + smoothMinimumX;
            smoothSource.insert(smoothSource.end(), begin, begin + smoothWidth);
        }
    }
    const auto smoothHeight = [&](const std::uint32_t x, const std::uint32_t z) {
        return smoothSource[static_cast<std::size_t>(z - smoothMinimumZ) * smoothWidth +
                            (x - smoothMinimumX)];
    };
    bool changed = false;
    TerrainRegion dirty{};
    for (std::uint32_t z = minimumZ; z <= maximumZ; ++z) {
        const float sampleZ = -depth * 0.5F + static_cast<float>(z) * spacingZ;
        for (std::uint32_t x = minimumX; x <= maximumX; ++x) {
            const float sampleX = -width * 0.5F + static_cast<float>(x) * spacingX;
            const float distance = std::hypot(sampleX - localX, sampleZ - localZ);
            if (distance > radius) continue;
            const float weight = brushFalloff(distance / radius, falloff);
            const std::size_t index = static_cast<std::size_t>(z) * resolution + x;
            const float sourceHeight = mode == TerrainSculptMode::Smooth
                                           ? smoothHeight(x, z) : heights[index];
            float target = sourceHeight;
            if (mode == TerrainSculptMode::Raise) {
                target += amount * weight;
            } else if (mode == TerrainSculptMode::Lower) {
                target -= amount * weight;
            } else if (mode == TerrainSculptMode::Flatten) {
                target = std::lerp(sourceHeight, flattenHeight, std::clamp(amount * weight, 0.0F, 1.0F));
            } else {
                float sum = 0.0F;
                std::uint32_t samples = 0;
                for (int offsetZ = -1; offsetZ <= 1; ++offsetZ) {
                    for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                        const int neighborX = static_cast<int>(x) + offsetX;
                        const int neighborZ = static_cast<int>(z) + offsetZ;
                        if (neighborX < 0 || neighborZ < 0 ||
                            neighborX >= static_cast<int>(resolution) ||
                            neighborZ >= static_cast<int>(resolution)) continue;
                        sum += smoothHeight(static_cast<std::uint32_t>(neighborX),
                                            static_cast<std::uint32_t>(neighborZ));
                        ++samples;
                    }
                }
                const float average = samples == 0 ? sourceHeight : sum / static_cast<float>(samples);
                target = std::lerp(sourceHeight, average, std::clamp(amount * weight, 0.0F, 1.0F));
            }
            target = std::clamp(target, minimumHeight, maximumHeight);
            if (std::abs(target - heights[index]) > std::numeric_limits<float>::epsilon()) {
                heights[index] = target;
                changed = true;
                if (!dirty.valid) {
                    dirty = {x, z, x, z, true};
                } else {
                    dirty.minimumX = std::min(dirty.minimumX, x);
                    dirty.minimumZ = std::min(dirty.minimumZ, z);
                    dirty.maximumX = std::max(dirty.maximumX, x);
                    dirty.maximumZ = std::max(dirty.maximumZ, z);
                }
            }
        }
    }
    if (changedRegion) *changedRegion = dirty;
    return changed;
}

} // namespace Engine
