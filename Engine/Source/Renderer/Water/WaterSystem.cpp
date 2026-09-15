#include "Engine/Renderer/Water/WaterSystem.h"
#include "Engine/Renderer/Water/VirtualWaterPageBuilder.h"

#include "Engine/ECS/Components/MeshRendererComponent.h"
#include "Engine/ECS/Components/TransformComponent.h"
#include "Engine/Scene/SceneEditor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <stdexcept>
#include <limits>

#include <glm/geometric.hpp>


namespace {
    struct AnalyticSurface final {
        float height{};
        Engine::Vec3 normal{0.0F, 1.0F, 0.0F};
        Engine::Vec3 velocity{};
    };

    [[nodiscard]] AnalyticSurface evaluateWaterSurface(const Engine::WaterBodyComponent& water,
                                                       const Engine::TransformComponent& transform,
                                                       const Engine::Vec3& worldPosition,
                                                       const float time,
                                                       const float baseHeight) {
        AnalyticSurface result{};
        result.height = baseHeight;
        glm::vec3 tangent{1.0F, 0.0F, 0.0F};
        glm::vec3 binormal{0.0F, 0.0F, 1.0F};
        glm::vec3 localVelocity{};
        const glm::mat4 worldMatrix = transform.worldMatrix().native();
        const glm::mat3 linearTransform{worldMatrix};
        const std::uint32_t count = std::min<std::uint32_t>(
            water.waveCount, static_cast<std::uint32_t>(water.waves.size()));
        for (std::uint32_t i = 0; i < count; ++i) {
            const Engine::WaterGerstnerWave& wave = water.waves[i];
            Engine::Vec2 direction = wave.direction;
            if (direction.length() < 1.0e-5F) direction = {1.0F, 0.0F};
            else direction = direction.normalized();
            const float k = 6.28318530718F / std::max(wave.wavelength, 1.0e-3F);
            const float phase = k * (direction.x() * worldPosition.x() +
                                     direction.y() * worldPosition.z()) + wave.speed * time;
            const float sine = std::sin(phase);
            const float cosine = std::cos(phase);
            const float amplitude = wave.amplitude;
            const float horizontal = wave.steepness * amplitude;
            const glm::vec3 localDisplacement{horizontal * direction.x() * cosine,
                                              amplitude * sine,
                                              horizontal * direction.y() * cosine};
            // Match the renderer: Gerstner is evaluated in world phase space, then
            // the authored local displacement is transformed by the water body's
            // complete world linear transform. This keeps rotated/scaled lakes and
            // rivers consistent with their rasterized surface.
            result.height += (linearTransform * localDisplacement).y;
            tangent += glm::vec3{-horizontal * direction.x() * direction.x() * k * sine,
                                 amplitude * direction.x() * k * cosine,
                                 -horizontal * direction.x() * direction.y() * k * sine};
            binormal += glm::vec3{-horizontal * direction.x() * direction.y() * k * sine,
                                  amplitude * direction.y() * k * cosine,
                                  -horizontal * direction.y() * direction.y() * k * sine};
            localVelocity += glm::vec3{-horizontal * direction.x() * wave.speed * sine,
                                       amplitude * wave.speed * cosine,
                                       -horizontal * direction.y() * wave.speed * sine};
        }
        const glm::vec3 worldTangent = glm::normalize(linearTransform * tangent);
        const glm::vec3 worldBinormal = glm::normalize(linearTransform * binormal);
        const glm::vec3 n = glm::normalize(glm::cross(worldBinormal, worldTangent));
        result.normal = Engine::Vec3{n};
        result.velocity = Engine::Vec3{linearTransform * localVelocity};
        return result;
    }

    [[nodiscard]] Engine::Vec3 transformPoint(const Engine::TransformComponent& transform,
                                              const Engine::Vec3& point) {
        const glm::vec4 p = transform.worldMatrix().native() * glm::vec4(point.native(), 1.0F);
        return Engine::Vec3{glm::vec3(p) / std::max(std::abs(p.w), 1.0e-6F)};
    }

    [[nodiscard]] bool pointInLake(const Engine::WaterBodyComponent& water,
                                   const Engine::TransformComponent& transform,
                                   const Engine::Vec3& worldPosition) {
        if (water.lakeBoundary.size() < 3) return false;
        bool inside = false;
        for (std::size_t i = 0, j = water.lakeBoundary.size() - 1; i < water.lakeBoundary.size(); j = i++) {
            const Engine::Vec3 a = transformPoint(transform, water.lakeBoundary[i]);
            const Engine::Vec3 b = transformPoint(transform, water.lakeBoundary[j]);
            const float dz = b.z() - a.z();
            const bool crosses = ((a.z() > worldPosition.z()) != (b.z() > worldPosition.z())) &&
                (worldPosition.x() < (b.x() - a.x()) * (worldPosition.z() - a.z()) /
                    (std::abs(dz) > 1.0e-6F ? dz : (dz >= 0.0F ? 1.0e-6F : -1.0e-6F)) + a.x());
            if (crosses) inside = !inside;
        }
        return inside;
    }

    [[nodiscard]] std::optional<float> lakeBaseHeight(const Engine::WaterBodyComponent& water,
                                                       const Engine::TransformComponent& transform,
                                                       const Engine::Vec3& worldPosition) {
        if (water.lakeBoundary.size() < 3) return std::nullopt;
        const Engine::Vec3 a3 = transformPoint(transform, water.lakeBoundary[0]);
        const glm::vec2 p{worldPosition.x(), worldPosition.z()};
        const glm::vec2 a{a3.x(), a3.z()};
        for (std::size_t i = 1; i + 1 < water.lakeBoundary.size(); ++i) {
            const Engine::Vec3 b3 = transformPoint(transform, water.lakeBoundary[i]);
            const Engine::Vec3 c3 = transformPoint(transform, water.lakeBoundary[i + 1]);
            const glm::vec2 b{b3.x(), b3.z()};
            const glm::vec2 c{c3.x(), c3.z()};
            const glm::vec2 v0 = b - a;
            const glm::vec2 v1 = c - a;
            const glm::vec2 v2 = p - a;
            const float det = v0.x * v1.y - v1.x * v0.y;
            if (std::abs(det) < 1.0e-8F) continue;
            const float invDet = 1.0F / det;
            const float u = (v2.x * v1.y - v1.x * v2.y) * invDet;
            const float v = (v0.x * v2.y - v2.x * v0.y) * invDet;
            const float w = 1.0F - u - v;
            constexpr float epsilon = 1.0e-4F;
            if (u >= -epsilon && v >= -epsilon && w >= -epsilon)
                return w * a3.y() + u * b3.y() + v * c3.y();
        }
        // buildLake() documents convex editor boundaries; numerical edge cases
        // fall back to their mean plane instead of the entity origin.
        float sum = 0.0F;
        for (const Engine::Vec3& local : water.lakeBoundary) sum += transformPoint(transform, local).y();
        return sum / static_cast<float>(water.lakeBoundary.size());
    }

    struct RiverHit final {
        bool hit{};
        float depth{};
        float flowSpeed{};
        float surfaceHeight{};
        Engine::Vec3 flowDirection{};
    };

    [[nodiscard]] RiverHit queryRiverFootprint(const Engine::WaterBodyComponent& water,
                                               const Engine::TransformComponent& transform,
                                               const Engine::Vec3& worldPosition) {
        RiverHit best{};
        float bestDistance2 = std::numeric_limits<float>::max();
        if (water.riverSpline.size() < 2) return best;
        const glm::vec2 p{worldPosition.x(), worldPosition.z()};
        for (std::size_t i = 0; i + 1 < water.riverSpline.size(); ++i) {
            const Engine::Vec3 aw = transformPoint(transform, water.riverSpline[i].position);
            const Engine::Vec3 bw = transformPoint(transform, water.riverSpline[i + 1].position);
            const glm::vec2 a{aw.x(), aw.z()}, b{bw.x(), bw.z()};
            const glm::vec2 ab = b - a;
            const float ab2 = glm::dot(ab, ab);
            const float t = ab2 > 1.0e-8F ? std::clamp(glm::dot(p - a, ab) / ab2, 0.0F, 1.0F) : 0.0F;
            const glm::vec2 closest = a + ab * t;
            const float d2 = glm::dot(p - closest, p - closest);
            const float width = std::lerp(water.riverSpline[i].width, water.riverSpline[i + 1].width, t) *
                                std::max(std::abs(transform.worldScale().x()), std::abs(transform.worldScale().z()));
            if (d2 > 0.25F * width * width || d2 >= bestDistance2) continue;
            bestDistance2 = d2; best.hit = true;
            best.depth = std::lerp(water.riverSpline[i].depth, water.riverSpline[i + 1].depth, t);
            best.flowSpeed = std::lerp(water.riverSpline[i].flowSpeed, water.riverSpline[i + 1].flowSpeed, t);
            best.surfaceHeight = std::lerp(aw.y(), bw.y(), t);
            const glm::vec2 dir = glm::dot(ab, ab) > 1.0e-8F ? glm::normalize(ab) : glm::vec2{1.0F, 0.0F};
            best.flowDirection = Engine::Vec3{dir.x, 0.0F, dir.y};
        }
        return best;
    }
}

namespace Engine {
    void WaterSystem::addWaterVertex(Mesh &mesh, const Vec3 &position, const Vec2 &uv, const float cellSize) {
        mesh.vertices.push_back({
            .position = position, .color = {1.0F, 1.0F, 1.0F}, .texCoord = uv,
            .normal = {0.0F, 1.0F, 0.0F}, .tangent = {1.0F, 0.0F, 0.0F, 1.0F},
            .texCoord1 = {cellSize, 0.0F}
        });
    }

    Mesh WaterSystem::buildOceanClipmap() {
        // Ocean geometry is now one reusable page grid.  The logical 448-page
        // domain lives in VirtualWaterRenderer and is never duplicated in the
        // vertex/index heap.  drawRanges are stitch topology variants, not pages.
        return Water::buildReusableOceanPageMesh();
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
                constexpr float finestCell = (2.0F * Water::OceanExtents[0]) / Water::ClipmapResolution;
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

    std::optional<WaterQueryResult> WaterSystem::query(Registry& registry, const Vec3& worldPosition,
                                                       const float time) {
        std::optional<WaterQueryResult> best;
        float bestSurface = -std::numeric_limits<float>::infinity();
        registry.view<WaterBodyComponent, TransformComponent>(
            [&](const Entity entity, const WaterBodyComponent& water, const TransformComponent& transform) {
                bool footprint = false;
                float authoredDepth = water.maxDepth;
                float flowSpeed = 0.0F;
                float baseHeight = transform.worldPosition().y();
                Vec3 flowDirection{};
                switch (water.type) {
                    case WaterBodyType::Ocean:
                        footprint = true;
                        break;
                    case WaterBodyType::Lake: {
                        footprint = pointInLake(water, transform, worldPosition);
                        if (footprint) {
                            const auto height = lakeBaseHeight(water, transform, worldPosition);
                            if (height.has_value()) baseHeight = *height;
                        }
                        break;
                    }
                    case WaterBodyType::River: {
                        const RiverHit river = queryRiverFootprint(water, transform, worldPosition);
                        footprint = river.hit;
                        authoredDepth = river.depth;
                        flowSpeed = river.flowSpeed;
                        flowDirection = river.flowDirection;
                        if (river.hit) baseHeight = river.surfaceHeight;
                        break;
                    }
                }
                if (!footprint) return;
                const AnalyticSurface surface = evaluateWaterSurface(water, transform, worldPosition, time, baseHeight);
                if (surface.height < bestSurface) return;
                bestSurface = surface.height;
                WaterQueryResult result{};
                result.surfaceHeight = surface.height;
                result.normal = surface.normal;
                result.velocity = surface.velocity + flowDirection * flowSpeed;
                result.depth = std::max(authoredDepth, 0.0F);
                result.immersion = std::max(surface.height - worldPosition.y(), 0.0F);
                result.flowSpeed = flowSpeed;
                result.waterBody = entity;
                result.type = water.type;
                best = result;
            });
        return best;
    }

} // namespace Engine
