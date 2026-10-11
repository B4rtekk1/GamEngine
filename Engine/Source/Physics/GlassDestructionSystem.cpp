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

        void prepare(GlassDestructionComponent &glass, glm::vec2 hit) {
            std::mt19937 random(glass.seed);
            std::uniform_real_distribution<float> unit(0.0F, 1.0F);
            const glm::vec2 half = glass.size * 0.5F;
            std::vector<glm::vec2> sites{hit};
            // Stratified seeds avoid tiny/sliver cells; one seed anchors the impact hole.
            const auto columns = static_cast<std::uint32_t>(std::ceil(std::sqrt(static_cast<float>(glass.maxFragments))));
            const auto rows = (glass.maxFragments + columns - 1) / columns;
            for (std::uint32_t i = 1; i < glass.maxFragments; ++i) {
                const glm::vec2 uv{(static_cast<float>(i % columns) + 0.2F + unit(random) * 0.6F) / static_cast<float>(columns),
                    (static_cast<float>(i / columns) + 0.2F + unit(random) * 0.6F) / static_cast<float>(rows)};
                const auto site = uv * glass.size - half;
                if (glm::distance(site, hit) > Epsilon) sites.push_back(site);
            }
            for (std::size_t i = 0; i < sites.size(); ++i) {
                std::vector<glm::vec2> polygon{{-half.x, -half.y}, {half.x, -half.y}, {half.x, half.y}, {-half.x, half.y}};
                for (std::size_t j = 0; j < sites.size() && polygon.size() >= 3; ++j) {
                    if (i == j) continue;
                    const auto normal = sites[j] - sites[i];
                    polygon = clip(polygon, normal, 0.5F * (glm::dot(sites[j], sites[j]) - glm::dot(sites[i], sites[i])));
                }
                if (polygon.size() < 3) continue;
                GlassFragment fragment;
                fragment.polygon = std::move(polygon);
                float twiceArea = 0.0F;
                glm::vec2 centroid{};
                for (std::size_t k = 0; k < fragment.polygon.size(); ++k) {
                    const auto a = fragment.polygon[k], b = fragment.polygon[(k + 1) % fragment.polygon.size()];
                    const float cross = a.x * b.y - a.y * b.x;
                    twiceArea += cross;
                    centroid += (a + b) * cross;
                    // Only an edge on the bottom mount can carry the pane.
                    // Top/side cells must remain connected to that mount.
                    fragment.frameSupported |= std::abs(a.y + half.y) < Epsilon &&
                        std::abs(b.y + half.y) < Epsilon && glm::distance(a, b) > Epsilon;
                }
                if (twiceArea <= Epsilon) continue;
                fragment.area = twiceArea * 0.5F;
                fragment.center = centroid / (3.0F * twiceArea);
                glass.fragments.push_back(std::move(fragment));
            }
            // Adjacent Voronoi cells share an edge, never just a corner.
            for (std::uint32_t i = 0; i < glass.fragments.size(); ++i)
                for (std::uint32_t j = i + 1; j < glass.fragments.size(); ++j) {
                    std::vector<glm::vec2> common;
                    for (auto a : glass.fragments[i].polygon)
                        for (auto b : glass.fragments[j].polygon)
                            if (glm::distance(a, b) < Epsilon &&
                                std::ranges::none_of(common, [&](auto p) { return glm::distance(p, a) < Epsilon; }))
                                common.push_back(a);
                    if (common.size() >= 2 && glm::distance(common[0], common[1]) > Epsilon)
                        glass.bonds.push_back({i, j, (common[0] + common[1]) * 0.5F, 0.0F});
                }
            glass.fractured = true;
        }

        void appendPrism(Mesh &mesh, const GlassFragment &fragment, float thickness, glm::vec2 origin = {}) {
            const auto &polygon = fragment.polygon;
            const float depth = thickness * 0.5F;
            auto triangle = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c) {
                const auto normal = glm::normalize(glm::cross(b - a, c - a));
                const auto start = static_cast<std::uint32_t>(mesh.vertices.size());
                for (auto p : {a, b, c}) {
                    Vertex vertex{};
                    vertex.position = Vec3{p};
                    vertex.color = {1.0F, 1.0F, 1.0F};
                    vertex.normal = Vec3{normal};
                    vertex.texCoord = Vec2{glm::vec2{p} + origin};
                    vertex.tangent = {1.0F, 0.0F, 0.0F, 1.0F};
                    mesh.vertices.push_back(vertex);
                }
                mesh.indices.insert(mesh.indices.end(), {start, start + 1, start + 2});
            };
            auto point = [&](std::size_t i, float z) { return glm::vec3{polygon[i] - origin, z}; };
            for (std::size_t i = 1; i + 1 < polygon.size(); ++i) {
                triangle(point(0, depth), point(i, depth), point(i + 1, depth));
                triangle(point(0, -depth), point(i + 1, -depth), point(i, -depth));
            }
            for (std::size_t i = 0; i < polygon.size(); ++i) {
                const auto j = (i + 1) % polygon.size();
                triangle(point(i, -depth), point(j, -depth), point(j, depth));
                triangle(point(i, -depth), point(j, depth), point(i, depth));
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
            auto &glass = registry.get<GlassDestructionComponent>(entity);
            const auto matrix = world.matrix().native();
            auto influence = [&](glm::vec2 point) {
                const auto displacement = glm::vec3{matrix * glm::vec4{point - hit, 0.0F, 0.0F}};
                return std::exp(-glm::dot(displacement, displacement) / (2.0F * impact.radius * impact.radius));
            };
            std::vector<bool> supported(glass.fragments.size());
            std::vector<std::uint32_t> queue;
            for (std::uint32_t i = 0; i < glass.fragments.size(); ++i)
                if (!forceUnsupported && glass.fragments[i].attached && glass.fragments[i].frameSupported) { supported[i] = true; queue.push_back(i); }
            std::vector<std::vector<std::uint32_t>> neighbors(glass.fragments.size());
            for (const auto &bond : glass.bonds) {
                if (bond.damage >= 1.0F) continue;
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
            std::vector<GlassFragment> detached;
            for (std::size_t i = 0; i < glass.fragments.size(); ++i) {
                auto &fragment = glass.fragments[i];
                if (fragment.attached && !supported[i]) fragment.attached = false;
                if (!fragment.attached && wasAttached[i]) detached.push_back(fragment);
            }
            glass.fullyDestroyed = std::ranges::none_of(glass.fragments, [](const auto &fragment) { return fragment.attached; });
            const auto collision = attachedMesh(glass);
            // The packed render geometry survives every subsequent impact. Only
            // section visibility changes; collision still needs the actual holes.
            registry.modify<MeshRendererComponent>(entity, [&](auto &renderer) {
                if (renderer.mesh.resource()->metadata.sections.size() != glass.fragments.size())
                    renderer.mesh = MeshHandle{GlassDestructionSystem::renderMesh(glass)};
            });
            const GlassDestructionComponent settings{
                .size = glass.size, .thickness = glass.thickness, .density = glass.density,
                .maxDynamicFragments = glass.maxDynamicFragments, .fragmentLifetime = glass.fragmentLifetime};
            const Material material = registry.get<MeshRendererComponent>(entity).material;
            if (collision->empty()) registry.remove<ColliderComponent>(entity);
            else {
                if (!registry.has<ColliderComponent>(entity)) registry.add<ColliderComponent>(entity);
                registry.modify<ColliderComponent>(entity, [&](auto &collider) { collider.shape = MeshCollider{collision}; collider.offset = {}; });
            }
            registry.markChanged<GlassDestructionComponent>(entity);
            std::uint32_t active = 0;
            registry.view<GlassDebrisComponent>([&](Entity, const auto &) { ++active; });
            const float volumeScale = std::abs(world.scale.x() * world.scale.y() * world.scale.z());
            std::uint32_t ordinal = 0;
            for (const auto &fragment : detached) {
                if (active >= settings.maxDynamicFragments) break;
                auto shard = std::make_shared<Mesh>();
                appendPrism(*shard, fragment, settings.thickness, fragment.center);
                shard->recalculateLocalBounds();
                const auto debris = scene.createMesh("Glass debris " + std::to_string(entity) + " " + std::to_string(++ordinal), shard, material.pbr);
                const Entity debrisEntity = scene.findEntity(debris.id());
                Transform pose = world;
                pose.position = Vec3{glm::vec3{matrix * glm::vec4{fragment.center, 0.0F, 1.0F}}};
                registry.modify<Transform>(debrisEntity, [&](auto &transform) { transform = pose; });
                const float mass = std::max(0.001F, fragment.area * settings.thickness * settings.density * volumeScale);
                registry.add<ColliderComponent>(debrisEntity, ColliderComponent{.shape = MeshCollider{shard}, .offset = {}, .friction = 0.4F, .restitution = 0.15F});
                registry.add<RigidbodyComponent>(debrisEntity, RigidbodyComponent{.mass = mass});
                const float directionLength = impact.worldDirection.length();
                RigidbodyState state;
                const float speed = std::min(15.0F, std::sqrt(2.0F * impact.energy / (settings.density * settings.size.x * settings.size.y * settings.thickness * volumeScale)));
                if (directionLength > Epsilon) state.linearVelocity = impact.worldDirection * (speed * influence(fragment.center) / directionLength);
                if (impact.energy > 0.0F) state.angularVelocity = {37.0F * static_cast<float>(ordinal % 5), -53.0F, 29.0F * static_cast<float>(ordinal % 7)};
                registry.add<RigidbodyState>(debrisEntity, state);
                registry.add<GlassDebrisComponent>(debrisEntity, GlassDebrisComponent{settings.fragmentLifetime});
                ++active;
            }
        }

    }

    std::shared_ptr<Mesh> GlassDestructionSystem::collisionMesh(const GlassDestructionComponent &glass) {
        return attachedMesh(glass);
    }

    std::shared_ptr<Mesh> GlassDestructionSystem::renderMesh(const GlassDestructionComponent &glass) {
        auto mesh = std::make_shared<Mesh>();
        for (const auto &fragment : glass.fragments) {
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
        mesh->recalculateLocalBounds();
        return mesh;
    }

    Actor GlassDestructionSystem::createPane(Scene &scene, std::string name,
        GlassDestructionComponent settings, PBRMaterial material) {
        validate(settings);
        settings.fractured = settings.fullyDestroyed = settings.hadPhysicalSupport = false;
        settings.fragments.clear();
        settings.bonds.clear();
        GlassFragment rectangle;
        const auto half = settings.size * 0.5F;
        rectangle.polygon = {{-half.x, -half.y}, {half.x, -half.y}, {half.x, half.y}, {-half.x, half.y}};
        auto mesh = std::make_shared<Mesh>();
        appendPrism(*mesh, rectangle, settings.thickness);
        mesh->recalculateLocalBounds();
        material.transmission = std::max(material.transmission, 0.96F);
        material.thickness = settings.thickness;
        material.doubleSided = true;
        const Actor pane = scene.createMesh(std::move(name), mesh, material);
        const Entity entity = scene.findEntity(pane.id());
        scene.registry().add<GlassDestructionComponent>(entity, std::move(settings));
        scene.registry().add<ColliderComponent>(entity, ColliderComponent{.shape = MeshCollider{mesh}, .offset = {}});
        return pane;
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
            impact.energy < glass.fractureThreshold) return false;
        const Transform world = TransformSystem::worldTransform(registry, entity);
        const glm::mat4 matrix = world.matrix().native();
        const float determinant = glm::determinant(matrix);
        if (!std::isfinite(determinant) || std::abs(determinant) < Epsilon) return false;
        // The remaining pane requires triangle collision; a dynamic convex hull would fill its holes.
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
        if (!glass.fractured) prepare(glass, hit);
        const float strength = impact.energy / glass.fractureThreshold;
        auto influence = [&](glm::vec2 point) {
            const auto displacement = glm::vec3{matrix * glm::vec4{point - hit, 0.0F, 0.0F}};
            return std::exp(-glm::dot(displacement, displacement) / (2.0F * impact.radius * impact.radius));
        };
        std::vector<bool> wasAttached;
        for (const auto &fragment : glass.fragments) wasAttached.push_back(fragment.attached);
        for (auto &fragment : glass.fragments) {
            if (!fragment.attached) continue;
            fragment.damage += strength * influence(fragment.center);
            if (contains(fragment)) fragment.damage = std::max(fragment.damage, 1.0F);
            if (fragment.damage >= 1.0F) fragment.attached = false;
        }
        for (auto &bond : glass.bonds) bond.damage += strength * influence(bond.center);
        detachUnsupported(scene, registry, entity, world, impact, hit, wasAttached);
        return true;
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
            for (auto entity : panes) {
                auto &registry = scene.registry();
                const auto world = TransformSystem::worldTransform(registry, entity);
                auto &glass = registry.get<GlassDestructionComponent>(entity);
                bool supported = false;
                const auto gravity = physics->gravity();
                if (gravity.length() <= Epsilon) continue;
                const auto down = gravity * (1.0F / gravity.length());
                for (float x : {-0.8F, 0.0F, 0.8F}) {
                    const auto point = glm::vec3{world.matrix().native() *
                        glm::vec4{x * glass.size.x * 0.5F, -glass.size.y * 0.5F, 0.0F, 1.0F}};
                    const auto hit = physics->raycastPrepared(scene, Vec3{point} - down * 0.01F,
                        down, 0.06F, scene.findByEntity(entity)->objectId());
                    if (hit && !registry.has<GlassDebrisComponent>(scene.findEntity(hit->actor.id())))
                        supported = true;
                }
                if (supported && !glass.hadPhysicalSupport) {
                    glass.hadPhysicalSupport = true;
                    registry.markChanged<GlassDestructionComponent>(entity);
                }
                if (!supported && glass.hadPhysicalSupport) {
                    if (!glass.fractured) prepare(glass, {0.0F, -glass.size.y * 0.5F});
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
