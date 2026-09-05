#include "Engine/Scene/TransformSystem.h"

#include "Engine/Scene/Components/IdentityComponents.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace Engine {
    namespace {
        struct HierarchyCache {
            std::uint64_t structuralRevision{};
            std::uint64_t uuidRevision{};
            std::uint64_t parentRevision{};
            std::uint64_t transformRevision{};
            std::uint32_t traversalGeneration{};
            bool initialized{false};
            std::unordered_map<Entity, std::vector<Entity>> children;
            std::vector<std::uint32_t> dirtyGeneration;
            std::vector<std::uint32_t> resolvedGeneration;
            std::vector<std::uint32_t> visitingGeneration;
        };

        // Runtime-only adjacency avoids rebuilding UUID lookup and hierarchy on every tick.
        std::unordered_map<const Registry *, HierarchyCache> caches;

        void rebuildHierarchy(Registry &registry, HierarchyCache &cache) {
            std::unordered_map<UUID, Entity> byUuid;
            byUuid.reserve(registry.size());
            registry.view<UUIDComponent>([&](const Entity entity, const UUIDComponent &uuid) {
                byUuid.emplace(uuid.value, entity);
            });
            cache.children.clear();
            registry.view<ParentComponent>([&](const Entity entity, ParentComponent &link) {
                const auto parent = byUuid.find(link.parentUuid);
                link.runtimeParent = parent == byUuid.end() ? NullEntity : parent->second;
                if (link.runtimeParent != NullEntity && link.runtimeParent != entity) {
                    cache.children[link.runtimeParent].push_back(entity);
                }
            });
            cache.structuralRevision = registry.structuralRevision();
            cache.uuidRevision = registry.componentRevision<UUIDComponent>();
            cache.parentRevision = registry.componentRevision<ParentComponent>();
        }

        void decomposeWorldTrs(const Transform &transform) noexcept {
            glm::vec3 scale{}, translation{}, skew{};
            glm::quat rotation{};
            glm::vec4 perspective{};
            if (glm::decompose(transform.cachedWorldMatrix.native(), scale, rotation, translation,
                               skew, perspective)) {
                transform.cachedWorldRotation = Vec3{
                    glm::degrees(glm::eulerAngles(glm::conjugate(rotation)))};
                transform.cachedWorldScale = Vec3{scale};
            } else {
                transform.cachedWorldRotation = transform.rotation;
                transform.cachedWorldScale = transform.scale;
            }
            transform.worldTrsValid = true;
        }

        std::uint32_t nextTraversalGeneration(HierarchyCache &cache) {
            if (++cache.traversalGeneration != 0) return cache.traversalGeneration;

            // A wrap is practically unreachable, but clearing preserves the stamp invariant.
            std::fill(cache.dirtyGeneration.begin(), cache.dirtyGeneration.end(), 0U);
            std::fill(cache.resolvedGeneration.begin(), cache.resolvedGeneration.end(), 0U);
            std::fill(cache.visitingGeneration.begin(), cache.visitingGeneration.end(), 0U);
            return ++cache.traversalGeneration;
        }
    }

    const Vec3 &TransformComponent::worldRotation() const noexcept {
        if (!worldTrsValid) decomposeWorldTrs(*this);
        return cachedWorldRotation;
    }

    const Vec3 &TransformComponent::worldScale() const noexcept {
        if (!worldTrsValid) decomposeWorldTrs(*this);
        return cachedWorldScale;
    }

    TransformComponent TransformComponent::worldTransform() const noexcept {
        return TransformComponent{.position = worldPosition(),
                                  .rotation = worldRotation(),
                                  .scale = worldScale()};
    }

    void TransformSystem::invalidate(const Registry &registry) noexcept {
        caches.erase(&registry);
    }

    void TransformSystem::updateDirty(Registry &registry) {
        HierarchyCache &cache = caches[&registry];
        const bool hierarchyChanged = !cache.initialized ||
            cache.structuralRevision != registry.structuralRevision() ||
            cache.uuidRevision != registry.componentRevision<UUIDComponent>() ||
            cache.parentRevision != registry.componentRevision<ParentComponent>();

        const std::uint64_t observedTransformRevision = cache.transformRevision;
        const std::size_t entityCapacity = registry.entityIndexCapacity();
        if (cache.dirtyGeneration.size() < entityCapacity) {
            cache.dirtyGeneration.resize(entityCapacity);
            cache.resolvedGeneration.resize(entityCapacity);
            cache.visitingGeneration.resize(entityCapacity);
        }
        const std::uint32_t generation = nextTraversalGeneration(cache);
        bool hasDirty = false;
        const auto markDirty = [&](const Entity entity) {
            const std::uint32_t index = entityIndex(entity);
            if (index < cache.dirtyGeneration.size()) {
                cache.dirtyGeneration[index] = generation;
                hasDirty = true;
            }
        };
        if (hierarchyChanged) {
            rebuildHierarchy(registry, cache);
            registry.view<Transform>([&](const Entity entity, Transform &) { markDirty(entity); });
        } else {
            registry.forEachComponentChangedSince<Transform>(observedTransformRevision, markDirty);
        }
        cache.transformRevision = registry.componentRevision<Transform>();
        cache.initialized = true;
        if (!hasDirty) return;

        const auto resolve = [&](auto &&self, const Entity entity) -> void {
            const std::uint32_t index = entityIndex(entity);
            if (!registry.valid(entity) || !registry.has<Transform>(entity) ||
                index >= cache.resolvedGeneration.size() ||
                cache.resolvedGeneration[index] == generation) return;
            if (cache.visitingGeneration[index] == generation) return; // Invalid cycles are treated as roots.
            cache.visitingGeneration[index] = generation;

            Transform &transform = registry.get<Transform>(entity);
            Entity parent = NullEntity;
            if (registry.has<ParentComponent>(entity)) {
                parent = registry.get<ParentComponent>(entity).runtimeParent;
                if (parent != entity && registry.has<Transform>(parent)) self(self, parent);
                else parent = NullEntity;
            }
            const Transform *parentTransform = parent != NullEntity && registry.has<Transform>(parent)
                ? &registry.get<Transform>(parent) : nullptr;
            const std::uint64_t parentRevision = parentTransform == nullptr ? 0 : parentTransform->worldRevision();
            const bool changed = cache.dirtyGeneration[index] == generation || !transform.worldCacheValid ||
                transform.cachedParent != parent ||
                transform.cachedParentWorldRevision != parentRevision;
            if (changed) {
                transform.previousCachedWorldMatrix = transform.cachedWorldMatrix;
                transform.cachedWorldMatrix = parentTransform == nullptr ? transform.matrix() :
                    Mat4{parentTransform->worldMatrix().native() * transform.matrix().native()};
                transform.cachedWorldPosition = Vec3{glm::vec3{transform.cachedWorldMatrix.native()[3]}};
                transform.cachedParent = parent;
                transform.cachedParentWorldRevision = parentRevision;
                ++transform.cachedWorldRevision;
                transform.worldCacheValid = true;
                transform.worldTrsValid = false;
            }
            cache.visitingGeneration[index] = 0;
            cache.resolvedGeneration[index] = generation;
            if (const auto children = cache.children.find(entity); children != cache.children.end()) {
                for (const Entity child : children->second) self(self, child);
            }
        };
        if (hierarchyChanged) {
            registry.view<Transform>([&](const Entity entity, Transform &) { resolve(resolve, entity); });
        } else {
            registry.forEachComponentChangedSince<Transform>(observedTransformRevision,
                [&](const Entity entity) { resolve(resolve, entity); });
        }
    }

    const Mat4 &TransformSystem::worldMatrix(const Registry &registry, const Entity entity) {
        return registry.get<Transform>(entity).worldMatrix();
    }

    Transform TransformSystem::worldTransform(const Registry &registry, const Entity entity) {
        return registry.get<Transform>(entity).worldTransform();
    }
} // namespace Engine
