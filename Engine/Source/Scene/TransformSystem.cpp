#include "Engine/Scene/TransformSystem.h"

#include "Engine/Math/MatrixDecompose.h"
#include "Engine/Scene/Components/IdentityComponents.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <algorithm>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>
#include <unordered_map>
#include <vector>

namespace Engine {
namespace {
struct HierarchyCache {
    std::uint64_t uuidRevision{};
    std::uint64_t parentRevision{};
    std::uint64_t transformRevision{};
    std::uint32_t traversalGeneration{};
    bool initialized{false};
    std::unordered_map<Entity, std::vector<Entity>> children;
    // Indexed by entity index. Most changed transforms are leaves; checking
    // this byte skips a hash lookup in `children` for each of them.
    std::vector<std::uint8_t> hasChildren;
    std::vector<std::uint32_t> dirtyGeneration;
    std::vector<std::uint32_t> resolvedGeneration;
    std::vector<std::uint32_t> visitingGeneration;
    std::vector<Entity> changedWorldTransforms;
    // Every world-matrix change since logStart, tagged with its update's
    // serial (see worldTransformChangesSince).
    std::vector<TransformSystem::WorldTransformChange> changeLog;
    std::uint64_t changeSerial{};
    std::uint64_t logStart{};
    bool logStarted{false};
    // After trimming, the oldest retained update may have lost entries.
    bool logStartPartial{false};
};

// Serials are global, so a rebuilt cache (new registry contents) never reuses
// one a consumer stored before.
std::uint64_t nextChangeSerial = 0;
// Bounds the log; a consumer further behind rescans everything.
constexpr std::size_t MaxChangeLogEntries = std::size_t{1} << 20;

// Runtime-only adjacency avoids rebuilding UUID lookup and hierarchy on every tick.
//
// The cache is thread-local because scenes are loaded/serialized on worker
// threads while the main thread keeps rendering another registry. A shared
// map here was an unsynchronized data race (and the registry address could be
// recycled by a different scene). Registries are non-copyable; their stable
// instanceId is the key.
thread_local std::unordered_map<std::uint64_t, HierarchyCache> caches;

HierarchyCache& hierarchyCacheFor(const Registry& registry) {
    // Bound stale entries left behind by scenes that were destroyed on this
    // thread. Rebuilding one cache is cheap compared to leaking them forever.
    constexpr std::size_t MaxCachedRegistries = 8;
    const auto found = caches.find(registry.instanceId());
    if (found != caches.end()) {
        return found->second;
    }
    if (caches.size() >= MaxCachedRegistries) {
        caches.clear();
    }
    return caches[registry.instanceId()];
}

void rebuildHierarchy(Registry& registry, HierarchyCache& cache) {
    std::unordered_map<UUID, Entity> byUuid;
    byUuid.reserve(registry.size());
    registry.view<UUIDComponent>(
        [&](const Entity entity, const UUIDComponent& uuid) { byUuid.emplace(uuid.value, entity); });
    cache.children.clear();
    cache.hasChildren.assign(registry.entityIndexCapacity(), 0);
    registry.view<ParentComponent>([&](const Entity entity, ParentComponent& link) {
        const auto parent = byUuid.find(link.parentUuid);
        link.runtimeParent = parent == byUuid.end() ? NullEntity : parent->second;
        if (link.runtimeParent != NullEntity && link.runtimeParent != entity) {
            cache.children[link.runtimeParent].push_back(entity);
            cache.hasChildren[entityIndex(link.runtimeParent)] = 1;
        }
    });
    cache.uuidRevision = registry.componentRevision<UUIDComponent>();
    cache.parentRevision = registry.componentRevision<ParentComponent>();
}

void decomposeWorldTrs(const Transform& transform) noexcept {
    glm::vec3 scale{};
    glm::vec3 translation{};
    glm::quat rotation{};
    if (decomposeAffineTrs(transform.cachedWorldMatrix.native(), scale, rotation, translation)) {
        transform.cachedWorldRotation = Vec3{glm::degrees(glm::eulerAngles(glm::conjugate(rotation)))};
        transform.cachedWorldScale = Vec3{scale};
    } else {
        transform.cachedWorldRotation = transform.rotation;
        transform.cachedWorldScale = transform.scale;
    }
    transform.worldTrsValid = true;
}

std::uint32_t nextTraversalGeneration(HierarchyCache& cache) {
    if (++cache.traversalGeneration != 0) {
        return cache.traversalGeneration;
    }

    // A wrap is practically unreachable, but clearing preserves the stamp invariant.
    std::ranges::fill(cache.dirtyGeneration, 0U);
    std::ranges::fill(cache.resolvedGeneration, 0U);
    std::ranges::fill(cache.visitingGeneration, 0U);
    return ++cache.traversalGeneration;
}
} // namespace

const Vec3& TransformComponent::worldRotation() const noexcept {
    if (!worldTrsValid) {
        decomposeWorldTrs(*this);
    }
    return cachedWorldRotation;
}

const Vec3& TransformComponent::worldScale() const noexcept {
    if (!worldTrsValid) {
        decomposeWorldTrs(*this);
    }
    return cachedWorldScale;
}

TransformComponent TransformComponent::worldTransform() const noexcept {
    return TransformComponent{
        .position = worldPosition(),
        .rotation = worldRotation(),
        .scale = worldScale(),
    };
}

void TransformSystem::invalidate(const Registry& registry) noexcept {
    caches.erase(registry.instanceId());
}

void TransformSystem::updateDirty(Registry& registry) {
    HierarchyCache& cache = hierarchyCacheFor(registry);
    // structuralRevision changes on every add/remove (including unrelated
    // components). Rebuilding for those was O(N) per inspector edit. The
    // adjacency only depends on UUIDs and parent links: destroying an entity
    // removes both components and bumps their revisions, so real hierarchy
    // edits are still observed here.
    const bool hierarchyChanged = !cache.initialized || cache.uuidRevision != registry.componentRevision<UUIDComponent>() ||
                                  cache.parentRevision != registry.componentRevision<ParentComponent>();

    const std::uint64_t observedTransformRevision = cache.transformRevision;
    if (!hierarchyChanged && observedTransformRevision == registry.componentRevision<Transform>()) {
        return;
    }
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
        registry.view<Transform>([&](const Entity entity, Transform&) { markDirty(entity); });
    } else {
        registry.forEachComponentChangedSinceUnique<Transform>(observedTransformRevision, markDirty);
    }
    cache.initialized = true;
    if (!hasDirty) {
        cache.transformRevision = registry.componentRevision<Transform>();
        return;
    }
    cache.changedWorldTransforms.clear();
    const std::uint64_t serial = ++nextChangeSerial;
    if (!cache.logStarted) {
        cache.logStarted = true;
        cache.logStart = serial;
    }
    cache.changeSerial = serial;
    if (cache.changeLog.size() > MaxChangeLogEntries) {
        cache.changeLog.erase(cache.changeLog.begin(),
                              cache.changeLog.begin() + static_cast<std::ptrdiff_t>(cache.changeLog.size() / 2));
        cache.logStart = cache.changeLog.front().serial;
        cache.logStartPartial = true;
    }
    const Registry& readRegistry = registry;

    // One tryGet per component replaces the former has()+get() pairs, each
    // of which re-validated the entity and re-probed its pool.
    const auto resolve = [&](auto&& self, const Entity entity) -> void {
        const std::uint32_t index = entityIndex(entity);
        if (index >= cache.resolvedGeneration.size() || cache.resolvedGeneration[index] == generation) {
            return;
        }
        Transform* const transformPointer = registry.tryGet<Transform>(entity);
        if (transformPointer == nullptr) {
            return;
        }
        if (cache.visitingGeneration[index] == generation) {
            return; // Invalid cycles are treated as roots.
        }
        cache.visitingGeneration[index] = generation;

        Transform& transform = *transformPointer;
        Entity parent = NullEntity;
        const Transform* parentTransform = nullptr;
        if (const auto* link = readRegistry.tryGet<ParentComponent>(entity); link != nullptr) {
            parent = link->runtimeParent;
            parentTransform = parent != entity ? readRegistry.tryGet<Transform>(parent) : nullptr;
            if (parentTransform != nullptr) {
                self(self, parent);
            } else {
                parent = NullEntity;
            }
        }
        const std::uint64_t parentRevision = parentTransform == nullptr ? 0 : parentTransform->worldRevision();
        const bool changed = cache.dirtyGeneration[index] == generation || !transform.worldCacheValid ||
                             transform.cachedParent != parent || transform.cachedParentWorldRevision != parentRevision;
        if (changed) {
            transform.cachedWorldMatrix =
                parentTransform == nullptr
                    ? transform.matrix()
                    : Mat4{parentTransform->worldMatrix().native() * transform.matrix().native()};
            transform.cachedWorldPosition = Vec3{glm::vec3{transform.cachedWorldMatrix.native()[3]}};
            transform.cachedParent = parent;
            transform.cachedParentWorldRevision = parentRevision;
            ++transform.cachedWorldRevision;
            transform.worldCacheValid = true;
            transform.worldTrsValid = false;
            cache.changedWorldTransforms.push_back(entity);
            cache.changeLog.push_back({serial, entity});
        }
        cache.visitingGeneration[index] = 0;
        cache.resolvedGeneration[index] = generation;
        // Resolving an unchanged ancestor must not visit all its other
        // branches. Explicitly dirty descendants are resolved by the
        // outer work list; only a changed world matrix propagates here.
        if (changed && index < cache.hasChildren.size() && cache.hasChildren[index] != 0) {
            if (const auto children = cache.children.find(entity); children != cache.children.end()) {
                for (const Entity child : children->second) {
                    self(self, child);
                }
            }
        }
    };
    if (hierarchyChanged) {
        registry.view<Transform>([&](const Entity entity, Transform&) { resolve(resolve, entity); });
    } else {
        registry.forEachComponentChangedSinceUnique<Transform>(
            observedTransformRevision, [&](const Entity entity) { resolve(resolve, entity); });
    }
    // Cache writes do not publish local Transform edits. Remember the
    // consumed revision and retain the world-change list for renderers.
    cache.transformRevision = registry.componentRevision<Transform>();
}

std::span<const Entity> TransformSystem::changedWorldTransforms(const Registry& registry) {
    const auto cache = caches.find(registry.instanceId());
    if (cache == caches.end()) {
        return {};
    }
    return cache->second.changedWorldTransforms;
}

std::uint64_t TransformSystem::worldTransformSerial(const Registry& registry) {
    const auto cache = caches.find(registry.instanceId());
    return cache == caches.end() ? 0 : cache->second.changeSerial;
}

std::span<const TransformSystem::WorldTransformChange>
TransformSystem::worldTransformChangesSince(const Registry& registry, const std::uint64_t serial, bool& complete) {
    const auto cache = caches.find(registry.instanceId());
    if (cache == caches.end() || !cache->second.logStarted) {
        // Nothing resolved yet; the first update reports every transform.
        complete = true;
        return {};
    }
    const HierarchyCache& entry = cache->second;
    complete = entry.logStartPartial ? serial >= entry.logStart : serial + 1 >= entry.logStart;
    const auto first = std::upper_bound(entry.changeLog.begin(), entry.changeLog.end(), serial,
                                        [](const std::uint64_t value, const WorldTransformChange& change) {
                                            return value < change.serial;
                                        });
    return {first, entry.changeLog.end()};
}

const Mat4& TransformSystem::worldMatrix(const Registry& registry, const Entity entity) {
    return registry.get<Transform>(entity).worldMatrix();
}

Transform TransformSystem::worldTransform(const Registry& registry, const Entity entity) {
    return registry.get<Transform>(entity).worldTransform();
}
} // namespace Engine
