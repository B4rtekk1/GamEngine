#pragma once

#include "Engine/Core/Transform.h"
#include "Engine/ECS/Registry.h"

namespace Engine {
    /** Resolves local ParentComponent transforms into cached world matrices. */
    class TransformSystem final {
    public:
        /** Updates only changed transforms and their descendants, parent first. */
        static void updateDirty(Registry &registry);

        /** Compatibility name for updateDirty(). */
        static void update(Registry &registry) { updateDirty(registry); }

        /** Returns an entity's cached world matrix in O(1); call update first. */
        [[nodiscard]] static const Mat4 &worldMatrix(const Registry &registry, Entity entity);

        /** Returns cached world-space TRS in O(1); call update first. */
        [[nodiscard]] static Transform worldTransform(const Registry &registry, Entity entity);
    };
} // namespace Engine
