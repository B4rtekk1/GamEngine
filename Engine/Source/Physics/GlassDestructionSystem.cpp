#include "Engine/Physics/GlassDestructionSystem.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/ECS/Components/MeshRendererComponent.h"
#include "Engine/ECS/Components/RigidbodyRuntime.h"
#include <algorithm>
#include <cmath>
#include <random>
#include <limits>
#include <stdexcept>
#include <map>
#include <array>
#include <glm/gtc/constants.hpp>
#include <chrono>
#include <mutex>
#include <bit>
#include "Engine/Core/Profiler.h"

namespace Engine {
    namespace {
        constexpr float Epsilon = 1.0e-5F;
        bool finite(glm::vec3 value) { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); }
        void validate(const GlassDestructionComponent &glass) {
            if (!std::isfinite(glass.size.x) || !std::isfinite(glass.size.y) ||
                glass.size.x < 0.01F || glass.size.y < 0.01F ||
                !std::isfinite(glass.thickness) || glass.thickness < 0.001F ||
                !std::isfinite(glass.density) || glass.density <= 0.0F ||
                !std::isfinite(glass.fractureThreshold) || glass.fractureThreshold <= 0.0F ||
                !std::isfinite(glass.fragmentLifetime) || glass.fragmentLifetime <= 0.0F ||
                static_cast<unsigned>(glass.type) > 3 || static_cast<unsigned>(glass.supportMode) > 2 ||
                glass.supportEdges > 15 || glass.layers == 0 || glass.layers > 16 ||
                !std::isfinite(glass.bondStrength) || glass.bondStrength <= 0.0F ||
                !std::isfinite(glass.crackWidth) || glass.crackWidth <= 0.0F ||
                glass.maxFragments < 4 || glass.maxFragments > 128 || glass.maxDynamicFragments > 128)
                throw std::invalid_argument("Invalid rectangular glass destruction settings");
        }

        bool enableLegacyPane(Registry &registry, Entity entity) {
            const auto &renderer = registry.get<MeshRendererComponent>(entity);
            if (renderer.material.pbr.transmission <= 0.0F || !renderer.mesh ||
                !registry.has<ColliderComponent>(entity)) return false;
            const auto &collider = registry.get<ColliderComponent>(entity);
            const auto *box = std::get_if<BoxCollider>(&collider.shape);
            if (!box || collider.isTrigger || collider.offset.length() > Epsilon) return false;
            if (registry.has<RigidbodyComponent>(entity) &&
                registry.get<RigidbodyComponent>(entity).type != RigidbodyType::Static) return false;

            // Legacy Glass presets are centered cube meshes flattened along Z.
            // Require the mesh bounds to match the box, so unrelated meshes with
            // a glass material cannot silently become rectangular panes.
            const AABB bounds = renderer.mesh.resource()->bounds;
            const auto center = (bounds.min.native() + bounds.max.native()) * 0.5F;
            const auto size = bounds.max.native() - bounds.min.native();
            if (!finite(size) || glm::length(center) > Epsilon ||
                glm::length(size * 0.5F - box->halfExtents.native()) > Epsilon ||
                size.x < 0.01F || size.y < 0.01F || size.z < 0.001F) return false;
            const auto source = renderer.mesh.source();
            if (!source || source->vertices.empty() || source->indices.empty()) return false;
            const auto half = size * 0.5F;
            if (std::ranges::any_of(source->vertices, [&](const Vertex &vertex) {
                return glm::length(glm::abs(vertex.position.native()) - half) > Epsilon;
            })) return false;
            const Transform world = TransformSystem::worldTransform(registry, entity);
            const auto worldSize = size * world.scale.native();
            if (!finite(worldSize) || worldSize.x <= 0.0F || worldSize.y <= 0.0F || worldSize.z <= 0.0F ||
                worldSize.z > 0.05F * std::min(worldSize.x, worldSize.y)) return false;
            GlassDestructionComponent settings;
            settings.size = glm::vec2{size};
            // Thickness is local geometry, not the world-space PBR value. The
            // old preset's Z scale (e.g. 0.01) already supplies physical thickness.
            settings.thickness = size.z;
            registry.add<GlassDestructionComponent>(entity, std::move(settings));
            return true;
        }

        std::vector<glm::vec2> clip(const std::vector<glm::vec2> &polygon, glm::vec2 normal, float offset) {
            std::vector<glm::vec2> result;
            if (polygon.empty()) return result;
            glm::vec2 previous = polygon.back();
            float previousDistance = glm::dot(previous, normal) - offset;
            for (glm::vec2 point : polygon) {
                const float distance = glm::dot(point, normal) - offset;
                if ((distance <= 0.0F) != (previousDistance <= 0.0F))
                    result.push_back(previous + (point - previous) * (previousDistance / (previousDistance - distance)));
                if (distance <= 0.0F) result.push_back(point);
                previous = point;
                previousDistance = distance;
            }
            return result;
        }

        bool supportedEdge(const GlassDestructionComponent &glass, glm::vec2 a, glm::vec2 b) {
            const auto half = glass.size * 0.5F;
            const unsigned mask = glass.supportMode == GlassSupportMode::Bottom ? 1U :
                glass.supportMode == GlassSupportMode::Frame ? 15U : glass.supportEdges;
            const float tolerance = std::max(glass.size.x, glass.size.y) * Epsilon;
            return ((mask & 1U) && std::abs(a.y + half.y) < tolerance && std::abs(b.y + half.y) < tolerance) ||
                ((mask & 2U) && std::abs(a.x - half.x) < tolerance && std::abs(b.x - half.x) < tolerance) ||
                ((mask & 4U) && std::abs(a.y - half.y) < tolerance && std::abs(b.y - half.y) < tolerance) ||
                ((mask & 8U) && std::abs(a.x + half.x) < tolerance && std::abs(b.x + half.x) < tolerance);
        }

        void appendPrism(Mesh &mesh, const GlassFragment &fragment, float thickness, glm::vec2 origin);

        void prepare(GlassDestructionComponent &glass, glm::vec2 hit) {
            GE_PROFILE_SCOPE("FractureGenerationCPU");
            const auto generationStart = std::chrono::steady_clock::now();
            // Identical authored patterns share CPU geometry; Scene owns the GPU
            // handle cache so resources cannot leak between Vulkan contexts.
            using TemplateKey = std::array<std::uint32_t, 13>;
            static std::mutex templateMutex;
            static std::map<TemplateKey, std::weak_ptr<const GlassDestructionComponent>> templates;
            const TemplateKey key{std::bit_cast<unsigned>(glass.size.x), std::bit_cast<unsigned>(glass.size.y),
                std::bit_cast<unsigned>(glass.thickness), std::bit_cast<unsigned>(glass.fractureThreshold),
                std::bit_cast<unsigned>(glass.bondStrength), glass.maxFragments, glass.seed, unsigned(glass.type),
                unsigned(glass.supportMode), glass.supportEdges, glass.layers,
                std::bit_cast<unsigned>(hit.x), std::bit_cast<unsigned>(hit.y)};
            const std::lock_guard guard(templateMutex);
            if (const auto entry = templates.find(key); entry != templates.end()) {
                if (const auto pattern = entry->second.lock()) {
                    glass.fragments = pattern->fragments; glass.bonds = pattern->bonds;
                    glass.fractureRenderGeometry = pattern->fractureRenderGeometry;
                    glass.intactRenderGeometry = pattern->intactRenderGeometry;
                    glass.fractureTemplate = pattern;
                    glass.fractureGenerationCpuMs += std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-generationStart).count();
                    return;
                }
            }
            glass.fractureTemplate.reset(); glass.intactRenderGeometry.reset();
            // Radial/concentric template: O(cells), with exponentially graded ring
            // spacing. Shared edges are indexed once, rather than comparing cell pairs.
            glass.fractureRenderGeometry.reset();
            glass.fragments.clear();
            glass.bonds.clear();
            std::mt19937 random(glass.seed);
            std::uniform_real_distribution<float> unit(0.0F, 1.0F);
            const glm::vec2 half = glass.size * 0.5F;
            const auto rays = std::clamp(static_cast<unsigned>(std::sqrt(float(glass.maxFragments))), 4U, 16U);
            const auto rings = std::max(1U, glass.maxFragments / rays);
            const float outer = glm::length(glass.size) * 2.0F;
            std::vector<glm::vec2> directions;
            const float rotation = unit(random) * glm::two_pi<float>();
            for (unsigned i = 0; i < rays; ++i) {
                const float angle = rotation + glm::two_pi<float>() *
                    (float(i) + (unit(random) - 0.5F) * 0.25F) / float(rays);
                directions.push_back({std::cos(angle), std::sin(angle)});
            }
            if (glass.type != GlassType::Tempered) for (unsigned ring = 0; ring < rings; ++ring) {
                const auto radius = [&](unsigned r) {
                    const float t = float(r) / float(rings);
                    return outer * (std::exp(t * 3.5F) - 1.0F) / (std::exp(3.5F) - 1.0F);
                };
                const float inner = radius(ring), outside = radius(ring + 1);
                for (unsigned ray = 0; ray < rays; ++ray) {
                    const auto a = directions[ray], b = directions[(ray + 1) % rays];
                    std::vector<glm::vec2> polygon;
                    if (ring == 0) polygon = {hit, hit + a * outside, hit + b * outside};
                    else polygon = {hit + a * inner, hit + a * outside, hit + b * outside, hit + b * inner};
                    for (const auto boundary : std::array<glm::vec2, 4>{{{1,0},{-1,0},{0,1},{0,-1}}})
                        polygon = clip(polygon, boundary, glm::dot(glm::abs(boundary), half));
                    // Clipping a vertex on a boundary can duplicate that vertex.
                    std::vector<glm::vec2> clean;
                    for (auto point : polygon)
                        if (clean.empty() || glm::distance(clean.back(), point) > Epsilon) clean.push_back(point);
                    if (clean.size() > 1 && glm::distance(clean.front(), clean.back()) <= Epsilon) clean.pop_back();
                    if (clean.size() < 3) continue;
                    GlassFragment fragment;
                    if (glass.type == GlassType::Armored) fragment.layerDamage.assign(glass.layers, 0);
                    fragment.polygon = std::move(clean);
                    float twiceArea = 0.0F;
                    glm::vec2 centroid{};
                    for (std::size_t k = 0; k < fragment.polygon.size(); ++k) {
                        const auto a0 = fragment.polygon[k], b0 = fragment.polygon[(k + 1) % fragment.polygon.size()];
                        const float cross = a0.x * b0.y - a0.y * b0.x;
                        twiceArea += cross;
                        centroid += (a0 + b0) * cross;
                        fragment.frameSupported |= supportedEdge(glass, a0, b0);
                    }
                    if (twiceArea <= Epsilon * Epsilon) continue;
                    fragment.area = twiceArea * 0.5F;
                    fragment.center = centroid / (3.0F * twiceArea);
                    glass.fragments.push_back(std::move(fragment));
                }
            }
            if (glass.type == GlassType::Tempered) {
                const auto columns = std::clamp(static_cast<unsigned>(std::sqrt(float(glass.maxFragments) * glass.size.x/glass.size.y)),
                    2U, glass.maxFragments/2);
                const auto rows = glass.maxFragments/columns;
                std::vector<float> xs{-half.x}, ys{-half.y};
                for (unsigned i=1; i<columns; ++i) xs.push_back(-half.x + glass.size.x *
                    (float(i)+(unit(random)-0.5F)*0.35F)/float(columns));
                for (unsigned i=1; i<rows; ++i) ys.push_back(-half.y + glass.size.y *
                    (float(i)+(unit(random)-0.5F)*0.35F)/float(rows));
                xs.push_back(half.x); ys.push_back(half.y);
                for (unsigned y=0; y<rows; ++y) for (unsigned x=0; x<columns; ++x) {
                    GlassFragment fragment;
                    fragment.polygon = {{xs[x],ys[y]}, {xs[x+1],ys[y]}, {xs[x+1],ys[y+1]}, {xs[x],ys[y+1]}};
                    fragment.center = {(xs[x]+xs[x+1])*0.5F, (ys[y]+ys[y+1])*0.5F};
                    fragment.area = (xs[x+1]-xs[x])*(ys[y+1]-ys[y]);
                    for (unsigned edge=0; edge<4; ++edge)
                        fragment.frameSupported |= supportedEdge(glass, fragment.polygon[edge], fragment.polygon[(edge+1)%4]);
                    glass.fragments.push_back(std::move(fragment));
                }
            }
            using Key = std::array<std::int64_t, 4>;
            struct Edge { unsigned fragment; glm::vec2 a, b; };
            std::map<Key, Edge> edges;
            const double quantum = double(std::max(glass.size.x, glass.size.y)) * 1.0e-5;
            for (unsigned i = 0; i < glass.fragments.size(); ++i) {
                const auto &polygon = glass.fragments[i].polygon;
                for (std::size_t k = 0; k < polygon.size(); ++k) {
                    const auto a = polygon[k], b = polygon[(k + 1) % polygon.size()];
                    std::array<std::int64_t, 2> qa{std::llround(a.x / quantum), std::llround(a.y / quantum)},
                        qb{std::llround(b.x / quantum), std::llround(b.y / quantum)};
                    if (qb < qa) std::swap(qa, qb);
                    const Key key{qa[0], qa[1], qb[0], qb[1]};
                    auto [edge, inserted] = edges.emplace(key, Edge{i, a, b});
                    if (!inserted) {
                        const float length = glm::distance(a, b);
                        const float reference = std::sqrt(glass.size.x * glass.size.y / float(glass.maxFragments));
                        const float strength = glass.fractureThreshold * glass.bondStrength *
                            (glass.thickness / 0.006F) * length / std::max(reference, 0.001F);
                        glass.bonds.push_back({edge->second.fragment, i, (a + b) * 0.5F, 0.0F,
                            a, b, length, std::max(strength, 0.001F), 0.0F, false});
                    }
                }
            }
            auto pattern = std::make_shared<GlassDestructionComponent>(glass);
            pattern->fractureTemplate.reset(); pattern->cracks.clear(); pattern->impactCount = 0;
            (void)GlassDestructionSystem::renderMesh(*pattern);
            GlassFragment rectangle;
            rectangle.polygon = {{-half.x,-half.y},{half.x,-half.y},{half.x,half.y},{-half.x,half.y}};
            auto intact = std::make_shared<Mesh>();
            appendPrism(*intact, rectangle, glass.thickness, {});
            intact->recalculateLocalBounds();
            pattern->intactRenderGeometry = intact;
            glass.intactRenderGeometry = intact;
            glass.fragments = pattern->fragments;
            glass.fractureRenderGeometry = pattern->fractureRenderGeometry;
            glass.fractureTemplate = pattern;
            if (templates.size() >= 64) {
                for (auto entry = templates.begin(); entry != templates.end();) {
                    if (entry->second.expired()) entry = templates.erase(entry); else ++entry;
                }
            }
            if (templates.size() < 64 || templates.contains(key)) templates[key] = pattern;
            glass.fractureGenerationCpuMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-generationStart).count();
        }

        void appendPrism(Mesh &mesh, const GlassFragment &fragment, float thickness, glm::vec2 origin = {}) {
            const auto &polygon = fragment.polygon;
            const float depth = thickness * 0.5F;
            auto vertex = [&](glm::vec3 point, glm::vec3 normal, glm::vec3 tangent) {
                Vertex value{};
                value.position = Vec3{point};
                value.color = {1.0F, 1.0F, 1.0F};
                value.normal = Vec3{normal};
                value.texCoord = Vec2{glm::vec2{point} + origin};
                value.tangent = {tangent.x, tangent.y, tangent.z, 1.0F};
                mesh.vertices.push_back(value);
            };
            const auto start = static_cast<std::uint32_t>(mesh.vertices.size());
            const auto n = static_cast<std::uint32_t>(polygon.size());
            auto point = [&](std::size_t i, float z) { return glm::vec3{polygon[i] - origin, z}; };
            for (unsigned i = 0; i < n; ++i) vertex(point(i, depth), {0,0,1}, {1,0,0});
            for (unsigned i = 0; i < n; ++i) vertex(point(i, -depth), {0,0,-1}, {1,0,0});
            for (unsigned i = 1; i + 1 < n; ++i) {
                mesh.indices.insert(mesh.indices.end(), {start, start+i, start+i+1,
                    start+n, start+n+i+1, start+n+i});
            }
            for (unsigned i = 0; i < n; ++i) {
                const auto j = (i + 1) % n;
                const auto edge = glm::normalize(glm::vec3{polygon[j] - polygon[i], 0});
                const auto normal = glm::cross(edge, glm::vec3{0,0,1});
                const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
                vertex(point(i, -depth), normal, edge); vertex(point(j, -depth), normal, edge);
                vertex(point(j, depth), normal, edge); vertex(point(i, depth), normal, edge);
                mesh.indices.insert(mesh.indices.end(), {base, base+1, base+2, base, base+2, base+3});
            }
        }

        std::shared_ptr<Mesh> attachedMesh(const GlassDestructionComponent &glass) {
            auto mesh = std::make_shared<Mesh>();
            for (const auto &fragment : glass.fragments)
                if (fragment.attached) appendPrism(*mesh, fragment, glass.thickness);
            mesh->recalculateLocalBounds();
            return mesh;
        }
        void detachUnsupported(Scene &scene, Registry &registry, Entity entity, const Transform &world,
            const GlassImpact &impact, glm::vec2 hit, const std::vector<bool> &wasAttached,
            bool forceUnsupported = false) {
            GE_PROFILE_SCOPE("BondSolverCPU");
            const auto solverStart = std::chrono::steady_clock::now();
            const auto simulation = scene.glassSimulationSettings();
            if (!std::isfinite(simulation.minDynamicFragmentArea) || simulation.minDynamicFragmentArea < 0 ||
                !std::isfinite(simulation.kineticEnergyFraction) || simulation.kineticEnergyFraction < 0 || simulation.kineticEnergyFraction > 1 ||
                !std::isfinite(simulation.maxLinearSpeed) || simulation.maxLinearSpeed < 0 ||
                !std::isfinite(simulation.maxAngularSpeed) || simulation.maxAngularSpeed < 0 ||
                !std::isfinite(simulation.targetPhysicsCpuMs) || simulation.targetPhysicsCpuMs <= 0)
                throw std::invalid_argument("Invalid glass simulation budget");

            auto &glass = registry.get<GlassDestructionComponent>(entity);
            const auto matrix = world.matrix().native();
            auto influence = [&](glm::vec2 point) {
                const auto displacement = glm::vec3{matrix * glm::vec4{point - hit, 0.0F, 0.0F}};
                return std::exp(-glm::dot(displacement, displacement) / (2.0F * std::max(impact.radius * impact.radius, 1.0e-12F)));
            };
            std::vector<bool> supported(glass.fragments.size());
            std::vector<std::uint32_t> queue;
            for (std::uint32_t i = 0; i < glass.fragments.size(); ++i)
                if (!forceUnsupported && glass.fragments[i].attached && glass.fragments[i].frameSupported) { supported[i] = true; queue.push_back(i); }
            std::vector<std::vector<std::uint32_t>> neighbors(glass.fragments.size());
            for (const auto &bond : glass.bonds) {
                if (bond.broken || bond.damage >= 1.0F) continue;
                neighbors[bond.first].push_back(bond.second);
                neighbors[bond.second].push_back(bond.first);
            }
            for (std::size_t next = 0; next < queue.size(); ++next)
                for (auto neighbor : neighbors[queue[next]])
                    if (!supported[neighbor] && glass.fragments[neighbor].attached) {
                        supported[neighbor] = true;
                        queue.push_back(neighbor);
                    }
            // Keep the runtime state independent of ECS component storage while creating actors.
            std::vector<std::uint32_t> detached;
            for (std::size_t i = 0; i < glass.fragments.size(); ++i) {
                auto &fragment = glass.fragments[i];
                if (fragment.attached && !supported[i]) fragment.attached = false;
                if (!fragment.attached && wasAttached[i]) detached.push_back(static_cast<std::uint32_t>(i));
            }
            glass.fullyDestroyed = std::ranges::none_of(glass.fragments, [](const auto &fragment) { return fragment.attached; });
            // Keep render slots and geometry stable. PhysX reuses cached convex
            // cell shapes for the remaining static compound and dynamic shards.
            if (!detached.empty()) glass.renderState = GlassRenderState::Fractured;
            if (registry.get<MeshRendererComponent>(entity).mesh.resource()->metadata.sections.size() != glass.fragments.size() + 1)
                registry.modify<MeshRendererComponent>(entity, [&](auto &renderer) {
                    renderer.mesh = scene.glassGeometryHandle(GlassDestructionSystem::renderMesh(glass));
                });
            const GlassDestructionComponent settings{
                .size = glass.size, .thickness = glass.thickness, .density = glass.density,
                .maxDynamicFragments = glass.maxDynamicFragments, .fragmentLifetime = glass.fragmentLifetime};
            if (glass.fullyDestroyed) registry.remove<ColliderComponent>(entity);
            else {
                if (!registry.has<ColliderComponent>(entity)) registry.add<ColliderComponent>(entity);
                registry.modify<ColliderComponent>(entity, [&](auto &collider) { collider.shape = MeshCollider{registry.get<MeshRendererComponent>(entity).mesh.source()}; collider.offset = {}; });
            }
            registry.markChanged<GlassDestructionComponent>(entity);
            std::uint32_t active = 0, paneActive = 0;
            registry.view<GlassDebrisComponent>([&](Entity candidate, const auto &) {
                ++active;
                if (registry.has<GlassFragmentBindingComponent>(candidate) &&
                    registry.get<GlassFragmentBindingComponent>(candidate).paneEntity == entity) ++paneActive;
            });
            // Largest relevant shards take the rigid-body budget before small slivers.
            std::ranges::sort(detached, [&](auto a, auto b) { return
                registry.get<GlassDestructionComponent>(entity).fragments[a].area >
                registry.get<GlassDestructionComponent>(entity).fragments[b].area; });
            const float volumeScale = std::abs(world.scale.x() * world.scale.y() * world.scale.z());
            float weightSum = 0.0F;
            for (auto index : detached) {
                const auto &fragment = registry.get<GlassDestructionComponent>(entity).fragments[index];
                weightSum += std::max(0.001F, fragment.area * settings.thickness * settings.density * volumeScale) *
                    influence(fragment.center) * influence(fragment.center);
            }
            const float kineticBudget = std::max(0.0F, impact.energy *
                std::clamp(simulation.kineticEnergyFraction, 0.0F, 1.0F));
            const float velocityScale = weightSum > Epsilon ? std::sqrt(2.0F * kineticBudget / weightSum) : 0.0F;
            registry.get<GlassDestructionComponent>(entity).bondSolverCpuMs +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-solverStart).count();
            GE_PROFILE_SCOPE("DebrisSpawnCPU");
            const auto debrisStart = std::chrono::steady_clock::now();
            std::uint32_t ordinal = 0;
            for (auto fragmentIndex : detached) {
                const auto fragment = registry.get<GlassDestructionComponent>(entity).fragments[fragmentIndex];
                if (paneActive >= settings.maxDynamicFragments || active >= scene.glassDynamicFragmentBudget() ||
                    fragment.area * world.scale.x() * world.scale.y() < simulation.minDynamicFragmentArea ||
                    settings.maxDynamicFragments == 0) {
                    if (scene.glassParticleBursts().size() < 1024) {
                        const auto position = glm::vec3{matrix * glm::vec4{fragment.center, 0, 1}};
                        const float speed = std::min(std::max(simulation.maxLinearSpeed, 0.0F), velocityScale * influence(fragment.center));
                        const auto direction = impact.worldDirection.native();
                        const float magnitude = glm::length(direction);
                        const auto velocity = magnitude > Epsilon ? direction * (speed/magnitude) : glm::vec3{};
                        scene.queueGlassParticleBurst({position, velocity,
                            std::clamp(std::sqrt(fragment.area * world.scale.x() * world.scale.y()), 0.008F, 0.12F),
                            settings.fragmentLifetime, registry.get<GlassDestructionComponent>(entity).type == GlassType::Tempered ? 4U : 1U,
                            registry.get<GlassDestructionComponent>(entity).seed + fragmentIndex});
                    }
                    continue;
                }
                auto shard = fragment.collisionGeometry;
                if (!shard) {
                    auto source = std::make_shared<Mesh>();
                    appendPrism(*source, fragment, settings.thickness, fragment.center);
                    source->recalculateLocalBounds();
                    shard = source;
                }
                const auto debris = scene.createActor("Glass debris " + std::to_string(entity) + " " + std::to_string(++ordinal));
                const Entity debrisEntity = scene.findEntity(debris.id());
                Transform pose = world;
                pose.position = Vec3{glm::vec3{matrix * glm::vec4{fragment.center, 0.0F, 1.0F}}};
                registry.modify<Transform>(debrisEntity, [&](auto &transform) { transform = pose; });
                const float mass = std::max(0.001F, fragment.area * settings.thickness * settings.density * volumeScale);
                registry.add<ColliderComponent>(debrisEntity, ColliderComponent{.shape = MeshCollider{shard}, .offset = {}, .friction = 0.4F, .restitution = 0.15F});
                registry.add<RigidbodyComponent>(debrisEntity, RigidbodyComponent{.mass = mass});
                const float directionLength = impact.worldDirection.length();
                RigidbodyState state;
                const float speed = std::min(std::max(simulation.maxLinearSpeed, 0.0F), velocityScale * influence(fragment.center));
                glm::vec3 velocity{};
                if (directionLength > Epsilon) {
                    const auto forward = impact.worldDirection.native() / directionLength;
                    auto lateral = glm::vec3{matrix * glm::vec4{fragment.center-hit, 0, 0}};
                    lateral -= forward * glm::dot(lateral, forward);
                    const float length = glm::length(lateral);
                    if (length > Epsilon) lateral /= length;
                    velocity = glm::normalize(forward + lateral * 0.08F) * speed;
                }
                // Impulse applied at the nearest point on the shard to the hit.
                glm::vec2 application = hit;
                float nearest = std::numeric_limits<float>::max();
                bool inside = true;
                for (std::size_t i = 0; i < fragment.polygon.size(); ++i) {
                    const auto a = fragment.polygon[i], b = fragment.polygon[(i+1) % fragment.polygon.size()];
                    const auto edge = b-a, delta = hit-a;
                    inside &= edge.x * delta.y - edge.y * delta.x >= -Epsilon;
                    const auto point = a + edge * std::clamp(glm::dot(delta, edge) / glm::dot(edge,edge), 0.0F, 1.0F);
                    const float distance = glm::dot(point-hit, point-hit);
                    if (distance < nearest) { nearest = distance; application = point; }
                }
                if (inside) application = hit;
                const glm::vec3 r = glm::vec3{matrix * glm::vec4{application-fragment.center, 0, 0}};
                // Exact planar second moments, plus the extrusion thickness term.
                float xx = 0, yy = 0, xy = 0;
                for (std::size_t i = 0; i < fragment.polygon.size(); ++i) {
                    const auto a = (fragment.polygon[i]-fragment.center) * glm::vec2{world.scale.native()};
                    const auto b = (fragment.polygon[(i+1)%fragment.polygon.size()]-fragment.center) * glm::vec2{world.scale.native()};
                    const float cross = a.x*b.y-a.y*b.x;
                    xx += cross * (a.x*a.x+a.x*b.x+b.x*b.x) / 12.0F;
                    yy += cross * (a.y*a.y+a.y*b.y+b.y*b.y) / 12.0F;
                    xy += cross * (2*a.x*a.y+a.x*b.y+b.x*a.y+2*b.x*b.y) / 24.0F;
                }
                const float area = fragment.area * world.scale.x() * world.scale.y();
                const float depth2 = std::pow(settings.thickness * world.scale.z(), 2) / 12.0F;
                glm::mat3 inertia{0.0F};
                inertia[0][0] = mass * (yy/area + depth2);
                inertia[1][1] = mass * (xx/area + depth2);
                inertia[2][2] = mass * (xx+yy)/area;
                inertia[0][1] = inertia[1][0] = -mass * xy/area;
                glm::mat3 rotation{matrix};
                for (unsigned i=0; i<3; ++i) rotation[i] = glm::normalize(rotation[i]);
                const auto worldInertia = rotation * inertia * glm::transpose(rotation);
                glm::vec3 angular{};
                if (std::abs(glm::determinant(worldInertia)) > 1.0e-15F)
                    angular = glm::inverse(worldInertia) * glm::cross(r, mass * velocity);
                const float angularSpeed = glm::length(angular);
                if (angularSpeed > simulation.maxAngularSpeed && angularSpeed > Epsilon)
                    angular *= std::max(simulation.maxAngularSpeed, 0.0F) / angularSpeed;
                // Rotation shares the same shard energy allocation as translation.
                const float translation = mass * glm::dot(velocity, velocity);
                const float rotationEnergy = glm::dot(angular, worldInertia * angular);
                const float normalization = translation > 0 ? std::sqrt(translation / (translation + rotationEnergy)) : 0;
                state.linearVelocity = Vec3{velocity * normalization};
                state.angularVelocity = Vec3{glm::degrees(angular * normalization)};
                registry.add<RigidbodyState>(debrisEntity, state);
                registry.add<GlassDebrisComponent>(debrisEntity, GlassDebrisComponent{settings.fragmentLifetime});
                registry.add<GlassFragmentBindingComponent>(debrisEntity, GlassFragmentBindingComponent{
                    registry.get<UUIDComponent>(entity).value, fragmentIndex, entity});
                registry.get<GlassDestructionComponent>(entity).fragments[fragmentIndex].debrisEntity = debrisEntity;
                ++active;
                ++paneActive;
            }
            registry.get<GlassDestructionComponent>(entity).debrisSpawnCpuMs +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-debrisStart).count();
        }

    }

    std::shared_ptr<Mesh> GlassDestructionSystem::collisionMesh(const GlassDestructionComponent &glass) {
        return attachedMesh(glass);
    }

    std::shared_ptr<Mesh> GlassDestructionSystem::renderMesh(const GlassDestructionComponent &glass) {
        if (glass.fractureRenderGeometry) return glass.fractureRenderGeometry;
        auto mesh = std::make_shared<Mesh>();
        for (const auto &fragment : glass.fragments) {
            if (!fragment.collisionGeometry) {
                auto shard = std::make_shared<Mesh>();
                appendPrism(*shard, fragment, glass.thickness, fragment.center);
                shard->recalculateLocalBounds();
                fragment.collisionGeometry = std::move(shard);
            }
            const auto first = mesh->indexCount();
            const auto firstVertex = mesh->vertices.size();
            appendPrism(*mesh, fragment, glass.thickness);
            glm::vec3 low{std::numeric_limits<float>::max()}, high{std::numeric_limits<float>::lowest()};
            for (auto i = firstVertex; i < mesh->vertices.size(); ++i) {
                low = glm::min(low, mesh->vertices[i].position.native());
                high = glm::max(high, mesh->vertices[i].position.native());
            }
            mesh->renderSections.push_back({.firstIndex = first, .indexCount = mesh->indexCount() - first,
                .localBounds = {Vec3{low}, Vec3{high}}});
        }
        // Reserve the intact draw alongside fragment draws. Its stable final
        // section is the only visible one until fracture; impacts never swap meshes.
        GlassFragment rectangle;
        const auto half = glass.size * 0.5F;
        rectangle.polygon = {{-half.x,-half.y},{half.x,-half.y},{half.x,half.y},{-half.x,half.y}};
        const auto intactFirst = mesh->indexCount();
        appendPrism(*mesh, rectangle, glass.thickness);
        mesh->renderSections.push_back({.firstIndex = intactFirst,
            .indexCount = mesh->indexCount() - intactFirst,
            .localBounds = {Vec3{-half.x,-half.y,-glass.thickness*0.5F}, Vec3{half.x,half.y,glass.thickness*0.5F}}});
        mesh->recalculateLocalBounds();
        glass.fractureRenderGeometry = mesh;
        return mesh;
    }

    Actor GlassDestructionSystem::createPane(Scene &scene, std::string name,
        GlassDestructionComponent settings, PBRMaterial material) {
        validate(settings);
        settings.fractured = settings.fullyDestroyed = settings.hadPhysicalSupport = false;
        settings.cracks.clear(); settings.impactCount = 0; settings.accumulatedEnergy = 0;
        settings.renderState = GlassRenderState::Intact;
        settings.fragments.clear();
        settings.bonds.clear();
        prepare(settings, {});
        settings.fractured = false;
        // Populate immutable convex collision payloads before gameplay.
        (void)renderMesh(settings);
        const auto half = settings.size * 0.5F;
        auto mesh = renderMesh(settings);
        material.transmission = std::max(material.transmission, 0.96F);
        material.thickness = settings.thickness;
        material.doubleSided = true;
        const Actor pane = scene.createMesh(std::move(name), mesh, material);
        const Entity entity = scene.findEntity(pane.id());
        scene.registry().modify<MeshRendererComponent>(entity, [&](auto &renderer) { renderer.mesh = scene.glassGeometryHandle(mesh); });
        scene.registry().add<GlassDestructionComponent>(entity, std::move(settings));
        scene.registry().add<ColliderComponent>(entity, ColliderComponent{.shape = BoxCollider{Vec3{half.x, half.y, settings.thickness * 0.5F}}, .offset = {}});
        return pane;
    }

    bool GlassDestructionSystem::prepareLegacyPane(Registry &registry, Entity entity) {
        if (registry.has<GlassDestructionComponent>(entity) || !enableLegacyPane(registry, entity)) return false;
        auto &glass = registry.get<GlassDestructionComponent>(entity);
        prepare(glass, {});
        glass.fractured = false;
        registry.modify<MeshRendererComponent>(entity, [&](auto &renderer) {
            renderer.mesh = MeshHandle{renderMesh(glass)};
        });
        return true;
    }

    void GlassDestructionSystem::restoreGeometryCache(GlassDestructionComponent &glass) {
        if (glass.fragments.empty()) return;
        auto prepared = glass;
        prepare(prepared, {});
        if (prepared.fragments.size() != glass.fragments.size()) return;
        for (std::size_t i=0; i<glass.fragments.size(); ++i)
            if (glass.fragments[i].polygon != prepared.fragments[i].polygon) return;
        glass.fractureRenderGeometry = prepared.fractureRenderGeometry;
        glass.intactRenderGeometry = prepared.intactRenderGeometry;
        glass.fractureTemplate = prepared.fractureTemplate;
        for (std::size_t i=0; i<glass.fragments.size(); ++i)
            glass.fragments[i].collisionGeometry = prepared.fragments[i].collisionGeometry;
    }

    bool GlassDestructionSystem::onImpact(Scene &scene, Entity entity, const GlassImpact &impact) {
        auto &registry = scene.registry();
        if (!registry.valid(entity) || !registry.has<Transform>(entity) ||
            !registry.has<MeshRendererComponent>(entity)) return false;
        if (!registry.has<GlassDestructionComponent>(entity)) {
            // Existing saved Glass presets predate the component. Register only
            // compatible thin boxes, and leave weak/invalid shots read-only.
            if (!finite(impact.worldPosition.native()) || !finite(impact.worldDirection.native()) ||
                !std::isfinite(impact.energy) || impact.energy < GlassDestructionComponent{}.fractureThreshold ||
                !std::isfinite(impact.radius) || impact.radius <= 0.0F ||
                !enableLegacyPane(registry, entity)) return false;
        }
        auto &glass = registry.get<GlassDestructionComponent>(entity);
        validate(glass);
        if (glass.fullyDestroyed || !finite(impact.worldPosition.native()) || !finite(impact.worldDirection.native()) ||
            !std::isfinite(impact.energy) || !std::isfinite(impact.radius) || impact.radius <= 0.0F ||
            impact.energy < glass.fractureThreshold * 0.05F) return false;
        // Direct gameplay calls may precede the transform stage. Root TRS is
        // authoritative; resolve a hierarchy only when the pane has a parent.
        if (registry.has<ParentComponent>(entity)) TransformSystem::updateDirty(registry);
        const Transform world = registry.has<ParentComponent>(entity)
            ? TransformSystem::worldTransform(registry, entity) : registry.get<Transform>(entity);
        const glm::mat4 matrix = world.matrix().native();
        const float determinant = glm::determinant(matrix);
        if (!std::isfinite(determinant) || std::abs(determinant) < Epsilon) return false;
        // Attached cells form a static compound; a single dynamic convex hull would fill its holes.
        if (registry.has<RigidbodyComponent>(entity) &&
            registry.get<RigidbodyComponent>(entity).type != RigidbodyType::Static) return false;
        // Thin physical prisms under mirrored transforms are outside this prototype's contract.
        if (world.scale.x() <= 0.0F || world.scale.y() <= 0.0F || world.scale.z() <= 0.0F) return false;
        const auto local = glm::vec3{glm::inverse(matrix) * glm::vec4{impact.worldPosition.native(), 1.0F}};
        if (std::abs(local.x) > glass.size.x * 0.5F + Epsilon ||
            std::abs(local.y) > glass.size.y * 0.5F + Epsilon ||
            std::abs(local.z) > glass.thickness * 0.5F + 0.001F) return false;
        const glm::vec2 hit{local};
        auto contains = [&](const GlassFragment &fragment) {
            for (std::size_t i = 0; i < fragment.polygon.size(); ++i) {
                const auto a = fragment.polygon[i], edge = fragment.polygon[(i + 1) % fragment.polygon.size()] - a;
                const auto delta = hit - a;
                if (edge.x * delta.y - edge.y * delta.x < -Epsilon) return false;
            }
            return true;
        };
        if (glass.fractured && std::ranges::none_of(glass.fragments,
            [&](const auto &fragment) { return fragment.attached && contains(fragment); })) return false;
        ++glass.impactCount;
        if (glass.cracks.size() == 128) glass.cracks.erase(glass.cracks.begin());
        const float scale = std::max(Epsilon, std::min(world.scale.x(), world.scale.y()));
        glass.cracks.push_back({hit, std::max(impact.radius / scale, 0.05F) *
            (2.0F + std::sqrt(impact.energy / glass.fractureThreshold)),
            std::min(1.0F, impact.energy / glass.fractureThreshold),
            glass.seed + glass.impactCount * 1664525U});
        glass.renderState = glass.fractured ? glass.renderState : GlassRenderState::Cracked;
        glass.accumulatedEnergy = std::min(glass.accumulatedEnergy, std::numeric_limits<float>::max()-impact.energy) + impact.energy;
        registry.markChanged<GlassDestructionComponent>(entity);
        const float effectiveThreshold = glass.fractureThreshold *
            (glass.type == GlassType::Armored ? float(glass.layers) : 1.0F);
        if (glass.type == GlassType::Armored) {
            bool penetrated = false;
            for (auto &fragment : glass.fragments) {
                if (!fragment.attached || !contains(fragment)) continue;
                if (fragment.layerDamage.size() != glass.layers) fragment.layerDamage.assign(glass.layers, 0);
                float remaining = impact.energy;
                for (auto &damage : fragment.layerDamage) {
                    const float absorbed = std::min(remaining, std::max(0.0F, glass.fractureThreshold-damage));
                    damage += absorbed; remaining -= absorbed;
                }
                penetrated |= std::ranges::all_of(fragment.layerDamage, [&](float damage) { return damage >= glass.fractureThreshold; });
            }
            if (!penetrated) return true;
        } else if (impact.energy < effectiveThreshold && glass.accumulatedEnergy < effectiveThreshold) return true;
        // Laminated fragments remain retained by the interlayer until it ruptures.
        if (glass.type == GlassType::Laminated && glass.accumulatedEnergy < effectiveThreshold * 8.0F) return true;
        if (!glass.fractured) {
            // Physical template is prepared outside gameplay; new impacts add their
            // own visual crack patterns without changing existing shard topology.
            if (glass.fragments.empty()) prepare(glass, hit);
            glass.fractured = true;
        }
        const float strength = impact.energy / glass.fractureThreshold;
        auto influence = [&](glm::vec2 point) {
            const auto displacement = glm::vec3{matrix * glm::vec4{point - hit, 0.0F, 0.0F}};
            return std::exp(-glm::dot(displacement, displacement) / (2.0F * std::max(impact.radius * impact.radius, 1.0e-12F)));
        };
        std::vector<bool> wasAttached;
        for (const auto &fragment : glass.fragments) wasAttached.push_back(fragment.attached);
        std::vector<bool> hitCells(glass.fragments.size());
        for (std::size_t i = 0; i < glass.fragments.size(); ++i) {
            auto &fragment = glass.fragments[i];
            if (!fragment.attached) continue;
            hitCells[i] = contains(fragment);
            fragment.damage += strength * influence(fragment.center);
            if (fragment.frameSupported) {
                float capacity = 0, exposure = 0;
                const float reference = std::sqrt(glass.size.x*glass.size.y/float(glass.maxFragments));
                for (std::size_t edgeIndex=0; edgeIndex<fragment.polygon.size(); ++edgeIndex) {
                    const auto a = fragment.polygon[edgeIndex], b = fragment.polygon[(edgeIndex+1)%fragment.polygon.size()];
                    if (!supportedEdge(glass, a, b)) continue;
                    const auto edge = b-a;
                    const float length = glm::length(glm::vec3{matrix*glm::vec4{edge,0,0}});
                    const float edgeCapacity = glass.fractureThreshold*glass.bondStrength*glass.thickness*world.scale.z()/0.006F *
                        length/std::max(reference,0.001F);
                    const auto point = a+edge*std::clamp(glm::dot(hit-a,edge)/glm::dot(edge,edge),0.0F,1.0F);
                    capacity += edgeCapacity; exposure += edgeCapacity*influence(point);
                }
                if (capacity > 0) fragment.anchorDamage += impact.energy*exposure/(capacity*capacity);
            }
            if (hitCells[i] || fragment.anchorDamage >= 1.0F || glass.type == GlassType::Tempered) fragment.frameSupported = false;
        }
        for (auto &bond : glass.bonds) {
            if (bond.broken) continue;
            const glm::vec2 edge = bond.edgeEnd-bond.edgeStart;
            const float length2 = glm::dot(edge,edge);
            const auto closest = length2 > Epsilon * Epsilon ? bond.edgeStart + edge *
                std::clamp(glm::dot(hit-bond.edgeStart,edge)/length2, 0.0F, 1.0F) : bond.center;
            bond.accumulatedDamage += impact.energy * influence(closest);
            const float worldLength = glm::length(glm::vec3{matrix*glm::vec4{edge,0,0}});
            const float strengthScale = bond.length > Epsilon ? worldLength/bond.length * world.scale.z() : 1.0F;
            bond.damage = bond.accumulatedDamage / std::max(bond.strength*strengthScale, 0.001F);
            bond.broken = bond.damage >= 1.0F || hitCells[bond.first] || hitCells[bond.second] ||
                glass.type == GlassType::Tempered;
            if (bond.broken) bond.damage = std::max(1.0F, bond.damage);
        }
        // Isolated hit cells have no bonds; their anchor is removed above.
        detachUnsupported(scene, registry, entity, world, impact, hit, wasAttached);
        return true;
    }

    void GlassDestructionSystem::onActorDestroyed(Scene &scene, Entity entity) {
        auto &registry = scene.registry();
        if (registry.has<GlassFragmentBindingComponent>(entity)) {
            const auto binding = registry.get<GlassFragmentBindingComponent>(entity);
            if (registry.valid(binding.paneEntity) && registry.has<GlassDestructionComponent>(binding.paneEntity)) {
                auto &glass = registry.get<GlassDestructionComponent>(binding.paneEntity);
                if (binding.fragmentIndex < glass.fragments.size()) glass.fragments[binding.fragmentIndex].debrisEntity = NullEntity;
                registry.markChanged<GlassDestructionComponent>(binding.paneEntity);
            }
        }
        if (!registry.has<GlassDestructionComponent>(entity) || !registry.has<MeshRendererComponent>(entity)) return;
        const auto material = registry.get<MeshRendererComponent>(entity).material;
        std::vector<Entity> debris;
        for (const auto &fragment : registry.get<GlassDestructionComponent>(entity).fragments)
            if (registry.valid(fragment.debrisEntity)) debris.push_back(fragment.debrisEntity);
        // Removing a pane is rare. Promote surviving shards to standalone renderers
        // here so their lifetime stays independent of the pane.
        for (auto shard : debris) {
            const auto *collider = std::get_if<MeshCollider>(&registry.get<ColliderComponent>(shard).shape);
            if (collider && collider->mesh) {
                MeshRendererComponent renderer;
                renderer.mesh = MeshHandle{collider->mesh};
                renderer.material = material;
                renderer.materialOverride = true;
                registry.add<MeshRendererComponent>(shard, std::move(renderer));
            }
            registry.remove<GlassFragmentBindingComponent>(shard);
        }
    }

    void GlassDestructionSystem::update(Scene &scene, float deltaTime, const PhysicsSystem *physics) {
        if (!std::isfinite(deltaTime) || deltaTime <= 0.0F) return;
        // Observe physical supports before creating actors (ECS storage may move).
        // A declared bottom mount is used until a real support has been observed.
        // Once observed, removing/moving it releases the whole remaining pane.
        if (physics) {
            std::vector<Entity> panes;
            scene.registry().view<GlassDestructionComponent, Transform, MeshRendererComponent>(
                [&](Entity entity, const auto &glass, const auto &, const auto &) {
                if (!glass.fullyDestroyed) panes.push_back(entity);
            });
            const auto colliderRevision = scene.registry().componentRevision<ColliderComponent>();
            const auto transformRevision = scene.registry().componentRevision<Transform>();
            const auto structureRevision = scene.registry().structuralRevision();
            for (auto entity : panes) {
                auto &registry = scene.registry();
                const auto world = TransformSystem::worldTransform(registry, entity);
                auto &glass = registry.get<GlassDestructionComponent>(entity);
                if (glass.supportMode != GlassSupportMode::Bottom) continue;
                bool supported = false;
                const auto gravity = physics->gravity();
                if (gravity.length() <= Epsilon) continue;
                bool needsCheck = glass.supportColliderRevision == ~std::uint64_t{0} ||
                    glass.supportPaneMatrix != registry.get<Transform>(entity).worldMatrix().native() ||
                    glass.supportGravity != gravity.native();
                if (!glass.physicalSupports.empty()) {
                    const auto changed = registry.componentEntitiesChangedSince<ColliderComponent>(glass.supportColliderRevision);
                    for (const auto &support : glass.physicalSupports)
                        needsCheck |= !registry.valid(support.entity) || !registry.has<Transform>(support.entity) ||
                            !registry.has<ColliderComponent>(support.entity) ||
                            (registry.get<Transform>(support.entity).worldRevision() != support.worldRevision &&
                             registry.get<Transform>(support.entity).worldMatrix().native() != support.worldMatrix) ||
                            std::ranges::find(changed, support.entity) != changed.end();
                } else {
                    // An implicit mount observes newly added/moved static supports.
                    // Moving debris cannot invalidate every pane's support cache.
                    auto relevant = [&](Entity candidate) {
                        return registry.valid(candidate) && registry.has<ColliderComponent>(candidate) &&
                            !registry.has<GlassDebrisComponent>(candidate) &&
                            (!registry.has<RigidbodyComponent>(candidate) ||
                             registry.get<RigidbodyComponent>(candidate).type != RigidbodyType::Dynamic);
                    };
                    for (auto candidate : registry.componentEntitiesChangedSince<ColliderComponent>(glass.supportColliderRevision))
                        needsCheck |= relevant(candidate);
                    for (auto candidate : registry.componentEntitiesChangedSince<Transform>(glass.supportTransformRevision))
                        needsCheck |= relevant(candidate);
                }
                glass.supportColliderRevision = colliderRevision;
                glass.supportTransformRevision = transformRevision;
                glass.supportPaneWorldRevision = registry.get<Transform>(entity).worldRevision();
                glass.supportPaneMatrix = registry.get<Transform>(entity).worldMatrix().native();
                glass.supportGravity = gravity.native();
                if (!needsCheck) continue;
                glass.physicalSupports.clear();
                const auto down = gravity * (1.0F / gravity.length());
                for (float x : {-0.8F, 0.0F, 0.8F}) {
                    const auto point = glm::vec3{world.matrix().native() *
                        glm::vec4{x * glass.size.x * 0.5F, -glass.size.y * 0.5F, 0.0F, 1.0F}};
                    ++glass.supportRaycasts;
                    const auto hit = physics->raycastPrepared(scene, Vec3{point} - down * 0.01F,
                        down, 0.06F, scene.findByEntity(entity)->objectId());
                    if (hit && !registry.has<GlassDebrisComponent>(scene.findEntity(hit->actor.id()))) {
                        supported = true;
                        const auto support = scene.findEntity(hit->actor.id());
                        glass.physicalSupports.push_back({support, registry.get<Transform>(support).worldRevision(),
                            registry.get<Transform>(support).worldMatrix().native()});
                    }
                }
                glass.supportColliderRevision = colliderRevision;
                glass.supportTransformRevision = transformRevision;
                glass.supportStructureRevision = structureRevision;
                glass.supportGravity = gravity.native();
                if (supported && !glass.hadPhysicalSupport) {
                    glass.hadPhysicalSupport = true;
                    registry.markChanged<GlassDestructionComponent>(entity);
                }
                if (!supported && glass.hadPhysicalSupport) {
                    if (!glass.fractured) {
                        if (glass.fragments.empty()) prepare(glass, {0.0F, -glass.size.y * 0.5F});
                        glass.fractured = true;
                    }
                    std::vector<bool> attached;
                    for (const auto &fragment : glass.fragments) attached.push_back(fragment.attached);
                    detachUnsupported(scene, registry, entity, world, GlassImpact{.worldDirection = {}, .energy = 0.0F},
                        {}, attached, true);
                }
            }
        }
        std::vector<Entity> expired;
        scene.registry().view<GlassDebrisComponent>([&](Entity entity, auto &debris) {
            debris.remainingLifetime -= deltaTime;
            if (debris.remainingLifetime <= 0.0F) expired.push_back(entity);
        });
        for (Entity entity : expired) scene.destroy(entity);
    }
}
