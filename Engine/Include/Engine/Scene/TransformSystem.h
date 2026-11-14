#pragma once

#include "Engine/Core/Transform.h"
#include "Engine/ECS/Registry.h"

#include <cstdint>
#include <span>

namespace Engine {
/** Resolves local ParentComponent transforms into cached world matrices. */
class TransformSystem final {
  public:
    /** Updates only changed transforms and their descendants, parent first. */
    static void updateDirty(Registry& registry);

    /**
     * Entities whose cached world matrix was resolved during the latest
     * dirty update. Descendants affected by a changed parent are included.
     * The result remains available when a later update finds no work.
     */
    [[nodiscard]] static std::span<const Entity> changedWorldTransforms(const Registry& registry);

    /** One resolved world-matrix change; serials grow across all updates. */
    struct WorldTransformChange final {
        std::uint64_t serial;
        Entity entity;
    };
    /**
     * Serial of the latest update that changed a world matrix. A consumer
     * that runs less often than updateDirty() (or after another caller's
     * update in the same frame) stores it and later reads every change since
     * with worldTransformChangesSince(); changedWorldTransforms() only holds
     * the latest batch, so such a consumer silently missed earlier ones.
     */
    [[nodiscard]] static std::uint64_t worldTransformSerial(const Registry& registry);
    /**
     * Changes after `serial`, oldest first. `complete` is false when the log
     * no longer reaches back that far (or the hierarchy cache was rebuilt):
     * the caller must then treat every transform as changed.
     */
    [[nodiscard]] static std::span<const WorldTransformChange>
    worldTransformChangesSince(const Registry& registry, std::uint64_t serial, bool& complete);

    /** Discards runtime hierarchy data after the registry contents are replaced. */
    static void invalidate(const Registry& registry) noexcept;

    /** Compatibility name for updateDirty(). */
    static void update(Registry& registry) {
        updateDirty(registry);
    }

    /** Returns an entity's cached world matrix in O(1); call update first. */
    [[nodiscard]] static const Mat4& worldMatrix(const Registry& registry, Entity entity);

    /** Returns cached world-space TRS in O(1); call update first. */
    [[nodiscard]] static Transform worldTransform(const Registry& registry, Entity entity);
};
} // namespace Engine
