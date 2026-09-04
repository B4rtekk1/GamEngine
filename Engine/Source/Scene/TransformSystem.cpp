#include "Engine/Scene/TransformSystem.h"

#include "Engine/Scene/Components/IdentityComponents.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Engine {
    namespace {
        struct HierarchyCache {
            std::uint64_t structuralRevision{};
            std::uint64_t uuidRevision{};
            std::uint64_t parentRevision{};
            std::uint64_t transformRevision{};
            bool initialized{false};
            std::unordered_map<Entity, std::vector<Entity>> children;
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

        void cacheWorldTrs(Transform &transform) {
            glm::vec3 scale{}, translation{}, skew{};
            glm::quat rotation{};
            glm::vec4 perspective{};
            if (glm::decompose(transform.cachedWorldMatrix.native(), scale, rotation, translation,
                               skew, perspective)) {
                transform.cachedWorldPosition = Vec3{translation};
                transform.cachedWorldRotation = Vec3{
                    glm::degrees(glm::eulerAngles(glm::conjugate(rotation)))};
                transform.cachedWorldScale = Vec3{scale};
            } else {
                transform.cachedWorldPosition = transform.position;
                transform.cachedWorldRotation = transform.rotation;
                transform.cachedWorldScale = transform.scale;
            }
        }
    }

    void TransformSystem::updateDirty(Registry &registry) {
        HierarchyCache &cache = caches[&registry];
        const bool hierarchyChanged = !cache.initialized ||
            cache.structuralRevision != registry.structuralRevision() ||
            cache.uuidRevision != registry.componentRevision<UUIDComponent>() ||
            cache.parentRevision != registry.componentRevision<ParentComponent>();

        std::vector<Entity> dirty;
        if (hierarchyChanged) {
            rebuildHierarchy(registry, cache);
            registry.view<Transform>([&](const Entity entity, Transform &) { dirty.push_back(entity); });
        } else {
            dirty = registry.componentEntitiesChangedSince<Transform>(cache.transformRevision);
        }
        cache.transformRevision = registry.componentRevision<Transform>();
        cache.initialized = true;
        if (dirty.empty()) return;

        const std::unordered_set<Entity> explicitlyDirty{dirty.begin(), dirty.end()};
        std::unordered_set<Entity> resolved;
        std::unordered_set<Entity> visiting;
        const auto resolve = [&](auto &&self, const Entity entity) -> void {
            if (!registry.valid(entity) || !registry.has<Transform>(entity) || resolved.contains(entity)) return;
            if (!visiting.insert(entity).second) return; // Invalid cycles are treated as roots.

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
            const bool changed = explicitlyDirty.contains(entity) || !transform.worldCacheValid ||
                transform.cachedParent != parent ||
                transform.cachedParentWorldRevision != parentRevision;
            if (changed) {
                transform.previousCachedWorldMatrix = transform.cachedWorldMatrix;
                transform.cachedWorldMatrix = parentTransform == nullptr ? transform.matrix() :
                    Mat4{parentTransform->worldMatrix().native() * transform.matrix().native()};
                transform.cachedParent = parent;
                transform.cachedParentWorldRevision = parentRevision;
                ++transform.cachedWorldRevision;
                transform.worldCacheValid = true;
                cacheWorldTrs(transform);
            }
            visiting.erase(entity);
            resolved.insert(entity);
            if (const auto children = cache.children.find(entity); children != cache.children.end()) {
                for (const Entity child : children->second) self(self, child);
            }
        };
        for (const Entity entity : dirty) resolve(resolve, entity);
    }

    const Mat4 &TransformSystem::worldMatrix(const Registry &registry, const Entity entity) {
        return registry.get<Transform>(entity).worldMatrix();
    }

    Transform TransformSystem::worldTransform(const Registry &registry, const Entity entity) {
        return registry.get<Transform>(entity).worldTransform();
    }
} // namespace Engine
