#include "Engine/Renderer/Geometry/ProceduralCloud.h"

#include "Engine/Renderer/Geometry/Sphere.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Engine {
namespace {
    constexpr std::uint32_t MinimumPuffs = 1U;
    constexpr std::uint32_t MaximumPuffs = 128U;
    constexpr float MinimumDimension = 0.1F;
    constexpr float MinimumPuffRadius = 0.05F;

    class Random final {
    public:
        explicit Random(std::uint32_t seed) : state_(seed == 0U ? 1U : seed) {}

        [[nodiscard]] float unit() {
            state_ ^= state_ << 13U;
            state_ ^= state_ >> 17U;
            state_ ^= state_ << 5U;
            return static_cast<float>(state_) / static_cast<float>(UINT32_MAX);
        }

    private:
        std::uint32_t state_;
    };
}

Mesh ProceduralCloud::createMesh(const ProceduralCloudComponent& settings) {
    const std::uint32_t puffCount = std::clamp(settings.puffCount, MinimumPuffs, MaximumPuffs);
    const Vec3 dimensions{std::max(settings.dimensions.x(), MinimumDimension),
                          std::max(settings.dimensions.y(), MinimumDimension),
                          std::max(settings.dimensions.z(), MinimumDimension)};
    const float puffRadius = std::max(settings.puffRadius, MinimumPuffRadius);
    const Mesh puff = Sphere::createMesh(6U, 10U);

    Mesh cloud;
    cloud.vertices.reserve(puff.vertices.size() * puffCount);
    cloud.indices.reserve(puff.indices.size() * puffCount);
    Random random{settings.seed};
    for (std::uint32_t puffIndex = 0; puffIndex < puffCount; ++puffIndex) {
        // A square-root radial distribution concentrates puffs near the centre,
        // producing a coherent cloud bank instead of a rectangular scatter.
        const float angle = random.unit() * 6.28318530718F;
        const float distance = std::sqrt(random.unit());
        const float x = std::cos(angle) * distance * dimensions.x() * 0.5F;
        const float z = std::sin(angle) * distance * dimensions.z() * 0.5F;
        const float y = (random.unit() - 0.5F) * dimensions.y() *
                        (1.0F - distance * 0.55F);
        const float scale = puffRadius * (0.65F + random.unit() * 0.70F);
        const Vec3 puffScale{scale * (0.85F + random.unit() * 0.35F),
                             scale * (0.65F + random.unit() * 0.30F),
                             scale * (0.85F + random.unit() * 0.35F)};
        const std::uint32_t firstVertex = static_cast<std::uint32_t>(cloud.vertices.size());
        for (const Vertex& vertex : puff.vertices) {
            Vertex transformed = vertex;
            transformed.position = {x + vertex.position.x() * puffScale.x() * 2.0F,
                                    y + vertex.position.y() * puffScale.y() * 2.0F,
                                    z + vertex.position.z() * puffScale.z() * 2.0F};
            transformed.normal = Vec3{vertex.normal.x() / puffScale.x(),
                                      vertex.normal.y() / puffScale.y(),
                                      vertex.normal.z() / puffScale.z()}.normalized();
            transformed.color = {0.94F, 0.96F, 1.0F};
            cloud.vertices.push_back(transformed);
        }
        for (const std::uint32_t index : puff.indices) cloud.indices.push_back(firstVertex + index);
    }
    return cloud;
}
} // namespace Engine
