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

[[nodiscard]] float smoothFalloff(const float normalizedDistance) noexcept {
    const float value = std::clamp(1.0F - normalizedDistance, 0.0F, 1.0F);
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

Mesh TerrainComponent::createMesh() const {
    if (!valid()) throw std::logic_error("Cannot build an invalid terrain");

    Mesh mesh;
    const std::uint32_t cellCount = resolution - 1;
    mesh.vertices.reserve(static_cast<std::size_t>(cellCount) * cellCount * 4);
    mesh.indices.reserve(static_cast<std::size_t>(cellCount) * cellCount * 6);
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

    for (std::uint32_t z = 0; z < cellCount; ++z) {
        for (std::uint32_t x = 0; x < cellCount; ++x) {
            const float shade = ((x + z) & 1U) == 0U ? 0.82F : 0.52F;
            const Vec3 color{shade, shade, shade};
            const std::uint32_t first = mesh.vertexCount();
            const auto addVertex = [&](const std::uint32_t sampleX, const std::uint32_t sampleZ) {
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
            };
            addVertex(x, z);
            addVertex(x + 1, z);
            addVertex(x + 1, z + 1);
            addVertex(x, z + 1);
            mesh.indices.insert(mesh.indices.end(), {
                first, first + 2, first + 1,
                first + 2, first, first + 3,
            });
        }
    }
    return mesh;
}

bool TerrainComponent::sculpt(const float localX, const float localZ, const float radius,
                              const float amount, const TerrainSculptMode mode,
                              const float flattenHeight) {
    if (!valid() || radius <= 0.0F || amount <= 0.0F || !std::isfinite(localX) ||
        !std::isfinite(localZ) || !std::isfinite(radius) || !std::isfinite(amount)) return false;

    const std::vector<float> source = heights;
    const float spacingX = width / static_cast<float>(resolution - 1);
    const float spacingZ = depth / static_cast<float>(resolution - 1);
    bool changed = false;
    for (std::uint32_t z = 0; z < resolution; ++z) {
        const float sampleZ = -depth * 0.5F + static_cast<float>(z) * spacingZ;
        for (std::uint32_t x = 0; x < resolution; ++x) {
            const float sampleX = -width * 0.5F + static_cast<float>(x) * spacingX;
            const float distance = std::hypot(sampleX - localX, sampleZ - localZ);
            if (distance > radius) continue;
            const float weight = smoothFalloff(distance / radius);
            const std::size_t index = static_cast<std::size_t>(z) * resolution + x;
            float target = source[index];
            if (mode == TerrainSculptMode::Raise) {
                target += amount * weight;
            } else if (mode == TerrainSculptMode::Lower) {
                target -= amount * weight;
            } else if (mode == TerrainSculptMode::Flatten) {
                target = std::lerp(source[index], flattenHeight, std::clamp(amount * weight, 0.0F, 1.0F));
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
                        sum += source[static_cast<std::size_t>(neighborZ) * resolution +
                                      static_cast<std::uint32_t>(neighborX)];
                        ++samples;
                    }
                }
                const float average = samples == 0 ? source[index] : sum / static_cast<float>(samples);
                target = std::lerp(source[index], average, std::clamp(amount * weight, 0.0F, 1.0F));
            }
            target = std::clamp(target, minimumHeight, maximumHeight);
            if (std::abs(target - heights[index]) > std::numeric_limits<float>::epsilon()) {
                heights[index] = target;
                changed = true;
            }
        }
    }
    return changed;
}

} // namespace Engine
