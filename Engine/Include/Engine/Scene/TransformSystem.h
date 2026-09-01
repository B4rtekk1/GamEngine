#pragma once

#include "Engine/Core/Transform.h"
#include "Engine/ECS/Registry.h"

namespace Engine {
    /** Resolves local ParentComponent transforms into cached world matrices. */
    class TransformSystem final {
    public:
        /** Updates dirty world transforms in parent-before-child order. */
        static void update(Registry &registry);

        /** Returns an entity's cached world matrix, updating the hierarchy first. */
        [[nodiscard]] static const Mat4 &worldMatrix(Registry &registry, Entity entity);

        /** Returns the world-space TRS approximation used by systems such as PhysX. */
        [[nodiscard]] static Transform worldTransform(Registry &registry, Entity entity);
    };
} // namespace Engine
