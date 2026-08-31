#include "Engine/Physics/PhysicsSystem.h"
#include "Engine/Core/Transform.h"
#include "Engine/ECS/Components/ColliderComponent.h"
#include "Engine/ECS/Components/RigidbodyComponent.h"
#include "Engine/ECS/Components/TerrainGrassComponent.h"
#include "Engine/Renderer/Geometry/Mesh.h"
#include "Engine/Scene/Scene.h"

#include <PxPhysicsAPI.h>
#include <cooking/PxCooking.h>
#include <extensions/PxDefaultCpuDispatcher.h>
#include <extensions/PxDefaultStreams.h>
#include <extensions/PxExtensionsAPI.h>
#include <extensions/PxRigidBodyExt.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ranges>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Engine {
    namespace {
        constexpr float DegreesToRadians = 0.01745329251994329577F;
        constexpr float RadiansToDegrees = 57.295779513082320876F;
        constexpr float MinimumDimension = 1.0e-4F;
        constexpr physx::PxU32 PhysXVersion =
            (static_cast<physx::PxU32>(PX_PHYSICS_VERSION_MAJOR) << 24U) +
            (static_cast<physx::PxU32>(PX_PHYSICS_VERSION_MINOR) << 16U) +
            (static_cast<physx::PxU32>(PX_PHYSICS_VERSION_BUGFIX) << 8U);

        physx::PxVec3 toPhysX(const Vec3 &value) noexcept {
            return {value.x(), value.y(), value.z()};
        }

        Vec3 fromPhysX(const physx::PxVec3 &value) noexcept {
            return {value.x, value.y, value.z};
        }

        struct GrassSphere final {
            Entity entity{NullEntity};
            Vec3 center;
            Vec3 velocity;
            float radius{};
        };

        void trampleTerrainGrass(Registry& registry, const float deltaTime) {
            std::vector<GrassSphere> spheres;
            registry.view<Transform, ColliderComponent>(
                [&](const Entity entity, const Transform& transform, const ColliderComponent& collider) {
                    const auto* sphere = std::get_if<SphereCollider>(&collider.shape);
                    if (sphere == nullptr) return;
                    const glm::vec3 center = glm::vec3{transform.matrix().native() *
                        glm::vec4{collider.offset.native(), 1.0F}};
                    const float scale = std::max({std::abs(transform.scale.x()),
                                                  std::abs(transform.scale.y()),
                                                  std::abs(transform.scale.z())});
                    const Vec3 velocity = registry.has<RigidbodyComponent>(entity)
                        ? registry.get<RigidbodyComponent>(entity).linearVelocity : Vec3{};
                    spheres.push_back({entity, Vec3{center}, velocity, sphere->radius * scale});
                });
            if (spheres.empty()) return;
            std::vector<float> grassCoverage(spheres.size());

            std::vector<Entity> changedTerrains;
            registry.view<Transform, TerrainGrassComponent>(
                [&](const Entity entity, const Transform& terrainTransform, TerrainGrassComponent& grass) {
                    if (grass.instances.empty()) return;
                    grass.rebuildSpatialIndex();
                    const glm::mat4 terrainMatrix = terrainTransform.matrix().native();
                    const glm::mat4 inverseTerrain = glm::inverse(terrainMatrix);
                    const float terrainScale = std::max(0.001F, std::min(
                        std::abs(terrainTransform.scale.x()), std::abs(terrainTransform.scale.z())));
                    bool changed = false;

                    // Recover only blades which were previously pressed.  The
                    // exponential response is stable at any fixed timestep and
                    // makes a footprint soften naturally instead of remaining a
                    // hard, permanent decal.
                    constexpr float recoveryPerSecond = 0.75F;
                    const float recovery = std::exp(-recoveryPerSecond * deltaTime);
                    std::size_t recoveredCount = 0;
                    for (const std::size_t index : grass.recoveringInstances) {
                        if (index >= grass.instances.size()) continue;
                        auto& instance = grass.instances[index];
                        instance.trampled *= recovery;
                        if (instance.trampled <= 0.01F) {
                            instance.trampled = 0.0F;
                            if (index < grass.recoveringInstanceMarks.size())
                                grass.recoveringInstanceMarks[index] = 0;
                        } else {
                            grass.recoveringInstances[recoveredCount++] = index;
                        }
                        grass.markInstanceDirty(index);
                        changed = true;
                    }
                    grass.recoveringInstances.resize(recoveredCount);

                    for (std::size_t sphereIndex = 0; sphereIndex < spheres.size(); ++sphereIndex) {
                        const GrassSphere& sphere = spheres[sphereIndex];
                        constexpr float grassReach = 0.45F;
                        const Vec3 localCenter{glm::vec3{inverseTerrain *
                            glm::vec4{sphere.center.native(), 1.0F}}};
                        const float localRadius = (sphere.radius + grassReach) / terrainScale;
                        const auto minimumX = static_cast<std::int32_t>(std::floor(
                            (localCenter.x() - localRadius) / TerrainGrassComponent::SpatialCellSize));
                        const auto maximumX = static_cast<std::int32_t>(std::floor(
                            (localCenter.x() + localRadius) / TerrainGrassComponent::SpatialCellSize));
                        const auto minimumZ = static_cast<std::int32_t>(std::floor(
                            (localCenter.z() - localRadius) / TerrainGrassComponent::SpatialCellSize));
                        const auto maximumZ = static_cast<std::int32_t>(std::floor(
                            (localCenter.z() + localRadius) / TerrainGrassComponent::SpatialCellSize));
                        const glm::vec3 localVelocity = glm::vec3{inverseTerrain *
                            glm::vec4{sphere.velocity.native(), 0.0F}};
                        const float localSpeed = std::hypot(localVelocity.x, localVelocity.z);
                        for (std::int32_t cellX = minimumX; cellX <= maximumX; ++cellX) {
                            for (std::int32_t cellZ = minimumZ; cellZ <= maximumZ; ++cellZ) {
                                const auto cell = grass.spatialCells.find(
                                    TerrainGrassComponent::spatialKey(cellX, cellZ));
                                if (cell == grass.spatialCells.end()) continue;
                                for (const std::size_t index : cell->second) {
                                    auto& instance = grass.instances[index];
                                    const Vec3 worldPosition{glm::vec3{terrainMatrix *
                                        glm::vec4{instance.position.native(), 1.0F}}};
                                    const float influence = sphere.radius + grassReach;
                                    const Vec3 delta = worldPosition - sphere.center;
                                    const float horizontalDistance = std::hypot(delta.x(), delta.z());
                                    // Project the spherical contact volume onto
                                    // the terrain. This prevents objects high
                                    // above the grass from creating a footprint.
                                    const float footprintSquared = influence * influence - delta.y() * delta.y();
                                    if (footprintSquared <= 0.0F) continue;
                                    const float footprintRadius = std::sqrt(footprintSquared);
                                    if (horizontalDistance >= footprintRadius) continue;
                                    const float target = std::clamp(
                                        (1.0F - horizontalDistance / footprintRadius) * 1.35F, 0.0F, 1.0F);
                                    grassCoverage[sphereIndex] += target;

                                    float directionX = localSpeed > 1.0e-4F
                                        ? localVelocity.x : instance.position.x() - localCenter.x();
                                    float directionZ = localSpeed > 1.0e-4F
                                        ? localVelocity.z : instance.position.z() - localCenter.z();
                                    float directionLength = std::hypot(directionX, directionZ);
                                    if (directionLength < 1.0e-4F) {
                                        directionX = 1.0F;
                                        directionZ = 0.0F;
                                    } else {
                                        directionX /= directionLength;
                                        directionZ /= directionLength;
                                    }
                                    // Moving objects comb grass along their
                                    // trajectory; stationary contacts retain a
                                    // subtle radial response. Blend rather than
                                    // snap to keep the trail visually stable.
                                    const float blend = std::clamp(target * 1.5F, 0.2F, 1.0F);
                                    directionX = instance.bendX * (1.0F - blend) + directionX * blend;
                                    directionZ = instance.bendZ * (1.0F - blend) + directionZ * blend;
                                    directionLength = std::hypot(directionX, directionZ);
                                    if (directionLength > 1.0e-4F) {
                                        instance.bendX = directionX / directionLength;
                                        instance.bendZ = directionZ / directionLength;
                                    }
                                    instance.trampled = std::max(instance.trampled, target);
                                    grass.markRecovering(index);
                                    grass.markInstanceDirty(index);
                                    changed = true;
                                }
                            }
                        }
                    }
                    if (changed) changedTerrains.push_back(entity);
                });
            for (const Entity entity : changedTerrains)
                registry.markChanged<TerrainGrassComponent>(entity);

            // Grass applies rolling resistance only in the horizontal plane:
            // falling, jumping, and gravity must retain their normal response.
            constexpr float grassDragPerSecond = 1.2F;
            for (std::size_t i = 0; i < spheres.size(); ++i) {
                const float coverage = std::min(grassCoverage[i], 1.0F);
                if (coverage <= 0.0F || !registry.has<RigidbodyComponent>(spheres[i].entity)) continue;
                registry.modify<RigidbodyComponent>(spheres[i].entity, [&](auto& body) {
                    if (body.type != RigidbodyType::Dynamic) return;
                    const float damping = std::exp(-grassDragPerSecond * coverage * deltaTime);
                    body.linearVelocity.setX(body.linearVelocity.x() * damping);
                    body.linearVelocity.setZ(body.linearVelocity.z() * damping);
                    body.angularVelocity *= damping;
                });
            }
        }

        Quat transformRotation(const Transform &transform) {
            return Quat::angleAxis(transform.rotation.x() * DegreesToRadians, {1.0F, 0.0F, 0.0F}) *
                   Quat::angleAxis(transform.rotation.y() * DegreesToRadians, {0.0F, 1.0F, 0.0F}) *
                   Quat::angleAxis(transform.rotation.z() * DegreesToRadians, {0.0F, 0.0F, 1.0F});
        }

        physx::PxQuat toPhysX(const Quat &value) noexcept {
            return {value.x(), value.y(), value.z(), value.w()};
        }

        Quat fromPhysX(const physx::PxQuat &value) noexcept {
            return Quat{value.w, value.x, value.y, value.z};
        }

        physx::PxTransform toPhysX(const Transform &transform) {
            return {toPhysX(transform.position), toPhysX(transformRotation(transform))};
        }

        Vec3 eulerDegrees(const Quat &rotation) {
            const Vec3 axisX = rotation * Vec3{1.0F, 0.0F, 0.0F};
            const Vec3 axisY = rotation * Vec3{0.0F, 1.0F, 0.0F};
            const Vec3 axisZ = rotation * Vec3{0.0F, 0.0F, 1.0F};
            const float y = std::asin(std::clamp(axisZ.x(), -1.0F, 1.0F)); //NOLINT
            const float cosY = std::cos(y);
            const float x = std::abs(cosY) > 1.0e-5F //NOLINT
                                ? std::atan2(-axisZ.y(), axisZ.z())
                                : std::atan2(axisY.z(), axisY.y());
            const float z = std::abs(cosY) > 1.0e-5F //NOLINT
                                ? std::atan2(-axisY.x(), axisX.x())
                                : 0.0F;
            return {x * RadiansToDegrees, y * RadiansToDegrees, z * RadiansToDegrees};
        }

        Vec3 absoluteScale(const Transform &transform) noexcept {
            return {
                std::max(std::abs(transform.scale.x()), MinimumDimension),
                std::max(std::abs(transform.scale.y()), MinimumDimension),
                std::max(std::abs(transform.scale.z()), MinimumDimension)
            };
        }

        bool same(const Vec3 &lhs, const Vec3 &rhs) noexcept {
            return lhs.x() == rhs.x() && lhs.y() == rhs.y() && lhs.z() == rhs.z();
        }

        bool samePose(const Transform &lhs, const Transform &rhs) noexcept {
            return same(lhs.position, rhs.position) && same(lhs.rotation, rhs.rotation);
        }

        bool nonZero(const Vec3 &value) noexcept {
            return value.x() != 0.0F || value.y() != 0.0F || value.z() != 0.0F;
        }
    } // namespace

    struct PhysicsSystem::BroadPhaseCache final {
        struct ActorRecord final {
            physx::PxRigidActor *actor{};
            Transform lastTransform{};
            RigidbodyType bodyType{RigidbodyType::Static};
            Vec3 lastLinearVelocity;
            Vec3 lastAngularVelocity;
        };

        struct CookedMeshKey final {
            const Mesh* mesh{};
            Vec3 scale{};

            [[nodiscard]] bool operator==(const CookedMeshKey& other) const noexcept {
                return mesh == other.mesh && scale.x() == other.scale.x() &&
                       scale.y() == other.scale.y() && scale.z() == other.scale.z();
            }
        };

        struct CookedMeshKeyHash final {
            [[nodiscard]] std::size_t operator()(const CookedMeshKey& key) const noexcept {
                const auto combine = [](std::size_t seed, const std::size_t value) {
                    return seed ^ (value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U));
                };
                std::size_t result = std::hash<const Mesh*>{}(key.mesh);
                result = combine(result, std::hash<float>{}(key.scale.x()));
                result = combine(result, std::hash<float>{}(key.scale.y()));
                return combine(result, std::hash<float>{}(key.scale.z()));
            }
        };

        physx::PxDefaultAllocator allocator;
        physx::PxDefaultErrorCallback errorCallback;
        physx::PxFoundation *foundation{};
        physx::PxPhysics *physics{};
        physx::PxCookingParams cookingParameters{physx::PxTolerancesScale{}};
        physx::PxDefaultCpuDispatcher *dispatcher{};
        physx::PxScene *physicsScene{};
        Registry *registry{};
        std::uint64_t structuralRevision{};
        std::uint64_t colliderRevision{};
        std::uint64_t rigidbodyRevision{};
        std::unordered_map<Entity, ActorRecord> actors;
        std::unordered_map<CookedMeshKey, physx::PxConvexMesh*, CookedMeshKeyHash> convexMeshes;
        std::unordered_map<CookedMeshKey, physx::PxTriangleMesh*, CookedMeshKeyHash> triangleMeshes;

        BroadPhaseCache() {
            using namespace physx;
            foundation = PxCreateFoundation(PhysXVersion, allocator, errorCallback);
            if (foundation == nullptr) {
                fail("PxCreateFoundation failed");
            }

            PxTolerancesScale scale;
            scale.length = 1.0F;
            scale.speed = 9.81F; // NOLINT g=9.81
            physics = PxCreatePhysics(PhysXVersion, *foundation, scale, true, nullptr);
            if (physics == nullptr) {
                fail("PxCreatePhysics failed");
            }
            if (!PxInitExtensions(*physics, nullptr)) {
                fail("PxInitExtensions failed");
            }

            cookingParameters = PxCookingParams{scale};
            cookingParameters.meshPreprocessParams |= PxMeshPreprocessingFlag::eWELD_VERTICES;
            cookingParameters.meshWeldTolerance = 1.0e-4F; //NOLINT

            dispatcher = PxDefaultCpuDispatcherCreate(2);
            if (dispatcher == nullptr) {
                fail("PxDefaultCpuDispatcherCreate failed");
            }

            PxSceneDesc sceneDescription{scale};
            sceneDescription.gravity = {0.0F, -9.81F, 0.0F}; // NOLINT g=9.81
            sceneDescription.cpuDispatcher = dispatcher;
            sceneDescription.filterShader = PxDefaultSimulationFilterShader;
            sceneDescription.flags |= PxSceneFlag::eENABLE_ACTIVE_ACTORS;
            physicsScene = physics->createScene(sceneDescription);
            if (physicsScene == nullptr) {
                fail("PxPhysics::createScene failed");
            }
        }

        ~BroadPhaseCache() { shutdown(); }

        BroadPhaseCache(const BroadPhaseCache &) = delete;

        BroadPhaseCache &operator=(const BroadPhaseCache &) = delete;

        [[noreturn]] void fail(const char *message) {
            shutdown();
            throw std::runtime_error(message);
        }

        void shutdown() noexcept {
            releaseActors();
            releaseCookedMeshes();
            if (physicsScene != nullptr) {
                physicsScene->release();
                physicsScene = nullptr;
            }
            if (dispatcher != nullptr) {
                dispatcher->release();
                dispatcher = nullptr;
            }
            if (physics != nullptr) {
                PxCloseExtensions();
                physics->release();
                physics = nullptr;
            }
            if (foundation != nullptr) {
                foundation->release();
                foundation = nullptr;
            }
        }

        void releaseActors() noexcept {
            for (auto &record: actors | std::views::values) {
                if (record.actor != nullptr) {
                    record.actor->release();
                }
            }
            actors.clear();
        }

        void releaseCookedMeshes() noexcept {
            for (const auto& mesh : convexMeshes | std::views::values) {
                if (mesh != nullptr) mesh->release();
            }
            convexMeshes.clear();
            for (const auto& mesh : triangleMeshes | std::views::values) {
                if (mesh != nullptr) mesh->release();
            }
            triangleMeshes.clear();
        }

        [[nodiscard]] std::optional<Entity> entityForActor(const physx::PxRigidActor *actor) const {
            for (const auto &[entity, record] : actors) {
                if (record.actor == actor) {
                    return entity;
                }
            }
            return std::nullopt;
        }

        static physx::PxShape *attachGeometry(physx::PxRigidActor &actor,
                                       const physx::PxGeometry &geometry,
                                       const physx::PxMaterial &material,
                                       const physx::PxTransform &localPose,
                                       const bool trigger) {
            physx::PxShape *shape = physx::PxRigidActorExt::createExclusiveShape(
                actor, geometry, material);
            if (shape == nullptr) {
                return nullptr;
            }
            shape->setLocalPose(localPose);
            shape->setFlag(physx::PxShapeFlag::eSIMULATION_SHAPE, !trigger);
            shape->setFlag(physx::PxShapeFlag::eTRIGGER_SHAPE, trigger);
            return shape;
        }

        physx::PxConvexMesh *cookConvex(const std::vector<physx::PxVec3> &points) const {
            if (points.size() < 4) {
                return nullptr;
            }
            physx::PxConvexMeshDesc description;
            description.points.count = static_cast<physx::PxU32>(points.size());
            description.points.stride = sizeof(physx::PxVec3);
            description.points.data = points.data();
            description.flags = physx::PxConvexFlag::eCOMPUTE_CONVEX;
            description.vertexLimit = 255; //NOLINT uint8_t

            physx::PxDefaultMemoryOutputStream output;
            if (!PxCookConvexMesh(cookingParameters, description, output)) {
                return nullptr;
            }
            physx::PxDefaultMemoryInputData input{output.getData(), output.getSize()};
            return physics->createConvexMesh(input);
        }

        physx::PxTriangleMesh *cookTriangleMesh(const Mesh &mesh, const Vec3 &scale) const {
            if (mesh.vertices.empty() || mesh.indices.size() < 3) {
                return nullptr;
            }
            std::vector<physx::PxVec3> points;
            points.reserve(mesh.vertices.size());
            for (const Vertex &vertex: mesh.vertices) {
                points.emplace_back(vertex.position.x() * scale.x(),
                                    vertex.position.y() * scale.y(),
                                    vertex.position.z() * scale.z());
            }

            // Vulkan render meshes are authored with clockwise front faces,
            // whereas PhysX derives collision normals from counter-clockwise
            // triangle winding.  Supply the same surface with its winding
            // reversed so contacts are generated on the visible/top side.
            std::vector<std::uint32_t> indices;
            indices.reserve(mesh.indices.size());
            for (std::size_t index = 0; index < mesh.indices.size(); index += 3) {
                indices.push_back(mesh.indices[index]);
                indices.push_back(mesh.indices[index + 2]);
                indices.push_back(mesh.indices[index + 1]);
            }

            physx::PxTriangleMeshDesc description;
            description.points.count = static_cast<physx::PxU32>(points.size());
            description.points.stride = sizeof(physx::PxVec3);
            description.points.data = points.data();
            description.triangles.count = static_cast<physx::PxU32>(mesh.indices.size() / 3);
            description.triangles.stride = sizeof(std::uint32_t) * 3;
            description.triangles.data = indices.data();

            physx::PxDefaultMemoryOutputStream output;
            if (!PxCookTriangleMesh(cookingParameters, description, output)) {
                return nullptr;
            }
            physx::PxDefaultMemoryInputData input{output.getData(), output.getSize()};
            return physics->createTriangleMesh(input);
        }

        physx::PxConvexMesh* cachedConvexMesh(const Mesh& mesh, const Vec3& scale) {
            const CookedMeshKey key{&mesh, scale};
            if (const auto existing = convexMeshes.find(key); existing != convexMeshes.end()) {
                return existing->second;
            }
            std::vector<physx::PxVec3> points;
            points.reserve(mesh.vertices.size());
            for (const Vertex& vertex : mesh.vertices) {
                points.emplace_back(vertex.position.x() * scale.x(), vertex.position.y() * scale.y(),
                                    vertex.position.z() * scale.z());
            }
            physx::PxConvexMesh* cooked = cookConvex(points);
            if (cooked != nullptr) convexMeshes.emplace(key, cooked);
            return cooked;
        }

        physx::PxTriangleMesh* cachedTriangleMesh(const Mesh& mesh, const Vec3& scale) {
            const CookedMeshKey key{&mesh, scale};
            if (const auto existing = triangleMeshes.find(key); existing != triangleMeshes.end()) {
                return existing->second;
            }
            physx::PxTriangleMesh* cooked = cookTriangleMesh(mesh, scale);
            if (cooked != nullptr) triangleMeshes.emplace(key, cooked);
            return cooked;
        }

        static bool attachBoundsFallback(physx::PxRigidActor &actor, const Mesh &mesh,
                                  const Vec3 &scale, const ColliderComponent &collider,
                                  physx::PxMaterial &material) {
            if (mesh.vertices.empty()) {
                return false;
            }
            Vec3 minimum = mesh.vertices.front().position;
            Vec3 maximum = minimum;
            for (const Vertex &vertex: mesh.vertices) {
                minimum = {
                    std::min(minimum.x(), vertex.position.x()),
                    std::min(minimum.y(), vertex.position.y()),
                    std::min(minimum.z(), vertex.position.z())
                };
                maximum = {
                    std::max(maximum.x(), vertex.position.x()),
                    std::max(maximum.y(), vertex.position.y()),
                    std::max(maximum.z(), vertex.position.z())
                };
            }
            const Vec3 center = minimum + maximum * 0.5F;
            const Vec3 extents = maximum - minimum * 0.5F * scale;
            const physx::PxBoxGeometry geometry{
                std::max(extents.x(), MinimumDimension),
                std::max(extents.y(), MinimumDimension),
                std::max(extents.z(), MinimumDimension)
            };
            return attachGeometry(actor, geometry, material,
                                  physx::PxTransform{toPhysX((center + collider.offset) * scale)}, //NOLINT
                                  collider.isTrigger) != nullptr;
        }

        // NOLINTNEXTLINE(readability-function-cognitive-complexity)
        bool attachCollider(physx::PxRigidActor &actor, const ColliderComponent &collider,
                            const Transform &transform, const bool dynamic) {
            using namespace physx;
            PxMaterial *material = physics->createMaterial(
                std::max(collider.friction, 0.0F), std::max(collider.friction, 0.0F),
                std::clamp(collider.restitution, 0.0F, 1.0F));
            if (material == nullptr) {
                return false;
            }

            const Vec3 scale = absoluteScale(transform);
            const PxTransform localOffset{toPhysX(collider.offset * scale)};
            bool attached = std::visit([&]<typename T>(const T &shape) {
                using Shape = std::decay_t<T>;
                if constexpr (std::is_same_v<Shape, BoxCollider>) {
                    const Vec3 extents = shape.halfExtents * scale;
                    const PxBoxGeometry geometry{
                        std::max(extents.x(), MinimumDimension),
                        std::max(extents.y(), MinimumDimension),
                        std::max(extents.z(), MinimumDimension)
                    };
                    return attachGeometry(actor, geometry, *material, localOffset,
                                          collider.isTrigger) != nullptr;
                } else if constexpr (std::is_same_v<Shape, SphereCollider>) {
                    const float radius = shape.radius *
                                         std::max({scale.x(), scale.y(), scale.z()});
                    const PxSphereGeometry geometry{std::max(radius, MinimumDimension)};
                    return attachGeometry(actor, geometry, *material, localOffset,
                                          collider.isTrigger) != nullptr;
                } else if constexpr (std::is_same_v<Shape, CapsuleCollider>) {
                    const float radius = shape.radius * std::max(scale.x(), scale.z());
                    const float totalHeight = shape.height * scale.y();
                    const float halfHeight = std::max((totalHeight * 0.5F) - radius, MinimumDimension);
                    const PxCapsuleGeometry geometry{
                        std::max(radius, MinimumDimension), halfHeight
                    };
                    const PxTransform capsulePose{
                        localOffset.p,
                        PxQuat{PxHalfPi, PxVec3{0.0F, 0.0F, 1.0F}}
                    };
                    return attachGeometry(actor, geometry, *material, capsulePose,
                                          collider.isTrigger) != nullptr;
                } else if constexpr (std::is_same_v<Shape, RampCollider>) {
                    const Vec3 extent = shape.halfExtents * scale;
                    const std::vector<PxVec3> points{
                        {-extent.x(), -extent.y(), -extent.z()},
                        {extent.x(), -extent.y(), -extent.z()},
                        {-extent.x(), -extent.y(), extent.z()},
                        {extent.x(), -extent.y(), extent.z()},
                        {-extent.x(), extent.y(), extent.z()},
                        {extent.x(), extent.y(), extent.z()},
                    };
                    PxConvexMesh *mesh = cookConvex(points);
                    if (mesh == nullptr) {
                        return false;
                    }
                    const PxConvexMeshGeometry geometry{mesh};
                    const bool result = attachGeometry(actor, geometry, *material, localOffset,
                                                       collider.isTrigger) != nullptr;
                    mesh->release();
                    return result;
                } else {
                    if (shape.mesh == nullptr || shape.mesh->vertices.empty()) {
                        return false;
                    }
                    if (dynamic) {
                        if (PxConvexMesh *mesh = cachedConvexMesh(*shape.mesh, scale); mesh != nullptr) {
                            const PxConvexMeshGeometry geometry{mesh};
                            const bool result = attachGeometry(
                                                    actor, geometry, *material, localOffset,
                                                    collider.isTrigger) != nullptr;
                            return result;
                        }
                        return attachBoundsFallback(
                            actor, *shape.mesh, scale, collider, *material);
                    }

                    PxTriangleMesh *mesh = cachedTriangleMesh(*shape.mesh, scale);
                    if (mesh == nullptr) {
                        return false;
                    }
                    PxTriangleMeshGeometry geometry{mesh};
                    // Render meshes use Vulkan's front-face convention, which
                    // need not match PhysX's one-sided triangle convention.
                    // Static mesh colliders must receive contacts from either
                    // side (notably for generated terrain).
                    geometry.meshFlags |= PxMeshGeometryFlag::eDOUBLE_SIDED;
                    const bool result = attachGeometry(actor, geometry, *material, localOffset,
                                                       collider.isTrigger) != nullptr;
                    return result;
                }
            }, collider.shape);
            material->release();
            return attached;
        }

        static void configureRigidBody(physx::PxRigidDynamic& rigid,
                                       const RigidbodyComponent& body) {
            using namespace physx;
            rigid.setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC,
                                   body.type == RigidbodyType::Kinematic);
            rigid.setActorFlag(PxActorFlag::eDISABLE_GRAVITY, !body.useGravity);
            rigid.setLinearDamping(std::max(body.linearDamping, 0.0F));
            rigid.setAngularDamping(std::max(body.angularDamping, 0.0F));
            PxRigidDynamicLockFlags locks;
            if (body.fixedRotation) {
                locks |= PxRigidDynamicLockFlag::eLOCK_ANGULAR_X;
                locks |= PxRigidDynamicLockFlag::eLOCK_ANGULAR_Y;
                locks |= PxRigidDynamicLockFlag::eLOCK_ANGULAR_Z;
            }
            rigid.setRigidDynamicLockFlags(locks);
            if (rigid.getNbShapes() == 0) {
                rigid.setMass(std::max(body.mass, MinimumDimension));
                rigid.setMassSpaceInertiaTensor({1.0F, 1.0F, 1.0F});
            } else {
                PxRigidBodyExt::setMassAndUpdateInertia(rigid, std::max(body.mass, MinimumDimension));
            }
            rigid.setLinearVelocity(toPhysX(body.linearVelocity));
            rigid.setAngularVelocity(toPhysX(body.angularVelocity * DegreesToRadians));
        }

        ActorRecord createActor(const Entity entity, Registry &owner, Transform &transform) {
            using namespace physx;
            RigidbodyComponent *body = owner.has<RigidbodyComponent>(entity)
                                           ? &owner.get<RigidbodyComponent>(entity)
                                           : nullptr;
            const bool dynamic = body != nullptr && body->type != RigidbodyType::Static;

            PxRigidActor *actor = dynamic
                                      ? static_cast<PxRigidActor *>(physics->createRigidDynamic(toPhysX(transform)))
                                      : static_cast<PxRigidActor *>(physics->createRigidStatic(toPhysX(transform)));
            if (actor == nullptr) {
                throw std::runtime_error("PhysX rigid actor creation failed");
            }
            if (owner.has<ColliderComponent>(entity)) {
                const ColliderComponent &collider = owner.get<ColliderComponent>(entity);
                if (!attachCollider(*actor, collider, transform, dynamic)) {
                    actor->release();
                    throw std::runtime_error("PhysX collider creation/cooking failed");
                }
            }

            ActorRecord record{
                .actor = actor,
                .lastTransform = transform,
                .bodyType = body == nullptr ? RigidbodyType::Static : body->type,
            };
            if (dynamic) {
                auto &rigid = *static_cast<PxRigidDynamic *>(actor);
                configureRigidBody(rigid, *body);
                record.lastLinearVelocity = body->linearVelocity;
                record.lastAngularVelocity = body->angularVelocity;
            }
            physicsScene->addActor(*actor);
            return record;
        }

        void rebuild(Registry &owner) {
            releaseActors();
            registry = &owner;
            owner.view<Transform>([&](const Entity entity, Transform &transform) {
                if (!owner.has<ColliderComponent>(entity) &&
                    !owner.has<RigidbodyComponent>(entity)) {
                    return;
                }
                actors.emplace(entity, createActor(entity, owner, transform));
            });
            structuralRevision = owner.structuralRevision();
            colliderRevision = owner.componentRevision<ColliderComponent>();
            rigidbodyRevision = owner.componentRevision<RigidbodyComponent>();
        }

        void removeActor(const Entity entity) noexcept {
            const auto record = actors.find(entity);
            if (record == actors.end()) return;
            if (record->second.actor != nullptr) record->second.actor->release();
            actors.erase(record);
        }

        void synchronizeActor(Registry& owner, const Entity entity) {
            if (!owner.valid(entity) || !owner.has<Transform>(entity) ||
                (!owner.has<ColliderComponent>(entity) && !owner.has<RigidbodyComponent>(entity))) {
                removeActor(entity);
                return;
            }
            removeActor(entity);
            Transform& transform = owner.get<Transform>(entity);
            actors.emplace(entity, createActor(entity, owner, transform));
        }

        void reconcileStructure(Registry& owner) {
            std::vector<Entity> stale;
            stale.reserve(actors.size());
            for (const auto& [entity, record] : actors) {
                if (!owner.valid(entity) || !owner.has<Transform>(entity) ||
                    (!owner.has<ColliderComponent>(entity) && !owner.has<RigidbodyComponent>(entity))) {
                    stale.push_back(entity);
                }
            }
            for (const Entity entity : stale) removeActor(entity);
            owner.view<Transform>([&](const Entity entity, Transform& transform) {
                if (actors.contains(entity) ||
                    (!owner.has<ColliderComponent>(entity) && !owner.has<RigidbodyComponent>(entity))) return;
                actors.emplace(entity, createActor(entity, owner, transform));
            });
        }

        void ensureWorld(Registry &owner) {
            if (registry != &owner) {
                rebuild(owner);
                return;
            }

            if (structuralRevision != owner.structuralRevision()) {
                reconcileStructure(owner);
                structuralRevision = owner.structuralRevision();
            }
            if (colliderRevision != owner.componentRevision<ColliderComponent>()) {
                for (const Entity entity : owner.componentEntitiesChangedSince<ColliderComponent>(colliderRevision)) {
                    synchronizeActor(owner, entity);
                }
                colliderRevision = owner.componentRevision<ColliderComponent>();
            }
            if (rigidbodyRevision != owner.componentRevision<RigidbodyComponent>()) {
                for (const Entity entity : owner.componentEntitiesChangedSince<RigidbodyComponent>(rigidbodyRevision)) {
                    const auto record = actors.find(entity);
                    if (!owner.valid(entity) || !owner.has<RigidbodyComponent>(entity) ||
                        record == actors.end() ||
                        record->second.bodyType != owner.get<RigidbodyComponent>(entity).type) {
                        synchronizeActor(owner, entity);
                    } else if (record->second.bodyType != RigidbodyType::Static) {
                        const RigidbodyComponent& body = owner.get<RigidbodyComponent>(entity);
                        configureRigidBody(*static_cast<physx::PxRigidDynamic*>(record->second.actor), body);
                        record->second.lastLinearVelocity = body.linearVelocity;
                        record->second.lastAngularVelocity = body.angularVelocity;
                    }
                }
                rigidbodyRevision = owner.componentRevision<RigidbodyComponent>();
            }

            std::vector<Entity> scaleChanged;
            for (const auto& [entity, record] : actors) {
                if (!same(record.lastTransform.scale, owner.get<Transform>(entity).scale)) {
                    scaleChanged.push_back(entity);
                }
            }
            for (const Entity entity : scaleChanged) synchronizeActor(owner, entity);
        }

        void pushEcsState(Registry &owner) {
            using namespace physx;
            for (auto &[entity, record]: actors) {
                Transform &transform = owner.get<Transform>(entity);
                if (!samePose(transform, record.lastTransform)) {
                    record.actor->setGlobalPose(toPhysX(transform), true);
                    record.lastTransform = transform;
                }

                if (!owner.has<RigidbodyComponent>(entity) ||
                    owner.get<RigidbodyComponent>(entity).type == RigidbodyType::Static) {
                    continue;
                }
                auto &body = owner.get<RigidbodyComponent>(entity);
                auto &rigid = *static_cast<PxRigidDynamic *>(record.actor);
                if (!same(body.linearVelocity, record.lastLinearVelocity)) {
                    rigid.setLinearVelocity(toPhysX(body.linearVelocity));
                }
                if (!same(body.angularVelocity, record.lastAngularVelocity)) {
                    rigid.setAngularVelocity(toPhysX(body.angularVelocity * DegreesToRadians));
                }
                if (body.type == RigidbodyType::Dynamic) {
                    if (nonZero(body.force)) {
                        rigid.addForce(toPhysX(body.force), PxForceMode::eFORCE);
                    }
                    if (nonZero(body.torque)) {
                        rigid.addTorque(toPhysX(body.torque), PxForceMode::eFORCE);
                    }
                    if (nonZero(body.angularImpulse)) {
                        rigid.addTorque(toPhysX(body.angularImpulse), PxForceMode::eIMPULSE);
                    }
                }
                body.zeroForces();
            }
        }

        void pullPhysXState(Registry &owner) {
            for (auto &[entity, record]: actors) {
                if (!owner.has<RigidbodyComponent>(entity)) {
                    continue;
                }
                auto &body = owner.get<RigidbodyComponent>(entity);
                if (body.type == RigidbodyType::Static) {
                    continue;
                }
                const auto &rigid = *static_cast<physx::PxRigidDynamic *>(record.actor);
                const physx::PxTransform pose = rigid.getGlobalPose();
                Transform &transform = owner.get<Transform>(entity);
                transform.position = fromPhysX(pose.p);
                transform.rotation = eulerDegrees(fromPhysX(pose.q).normalized());
                body.linearVelocity = fromPhysX(rigid.getLinearVelocity());
                body.angularVelocity = fromPhysX(rigid.getAngularVelocity()) * RadiansToDegrees;
                if (body.fixedRotation) {
                    body.angularVelocity = {};
                }
                record.lastTransform = transform;
                record.lastLinearVelocity = body.linearVelocity;
                record.lastAngularVelocity = body.angularVelocity;
                owner.markChanged<Transform>(entity);
            }
        }
    };

    void PhysicsSystem::update(Scene &scene, const float deltaTime) const {
        if (deltaTime <= 0.0F) {
            return;
        }
        if (!broadPhaseCache_) {
            broadPhaseCache_ = std::make_shared<BroadPhaseCache>();
        }
        BroadPhaseCache &runtime = *broadPhaseCache_;
        Registry &registry = scene.registry();
        runtime.ensureWorld(registry);
        runtime.physicsScene->setGravity(toPhysX(gravity_));
        runtime.pushEcsState(registry);
        runtime.physicsScene->simulate(deltaTime);
        runtime.physicsScene->fetchResults(true);
        runtime.pullPhysXState(registry);
        trampleTerrainGrass(registry, deltaTime);
    }

    std::optional<RaycastHit> PhysicsSystem::raycast(
        Scene &scene, const Vec3 origin, Vec3 direction, const float maxDistance) const {
        const float directionLength = direction.length();
        if (directionLength <= 0.0F || maxDistance <= 0.0F) {
            return std::nullopt;
        }
        direction *= 1.0F / directionLength;

        if (!broadPhaseCache_) {
            broadPhaseCache_ = std::make_shared<BroadPhaseCache>();
        }
        BroadPhaseCache &runtime = *broadPhaseCache_;
        Registry &registry = scene.registry();
        runtime.ensureWorld(registry);
        runtime.pushEcsState(registry);

        physx::PxRaycastBuffer hit;
        const physx::PxHitFlags hitFlags =
                physx::PxHitFlag::ePOSITION | physx::PxHitFlag::eNORMAL;
        if (!runtime.physicsScene->raycast(
                toPhysX(origin), toPhysX(direction), maxDistance, hit, hitFlags) ||
            !hit.hasBlock || hit.block.actor == nullptr) {
            return std::nullopt;
        }

        const std::optional<Entity> entity = runtime.entityForActor(hit.block.actor);
        if (!entity.has_value()) {
            return std::nullopt;
        }
        GameObject *object = scene.findByEntity(*entity);
        if (object == nullptr) {
            return std::nullopt;
        }
        return RaycastHit{
            .actor = Actor{scene, object->objectId()}, .point = fromPhysX(hit.block.position),
            .normal = fromPhysX(hit.block.normal), .distance = hit.block.distance,
        };
    }
} // namespace Engine
