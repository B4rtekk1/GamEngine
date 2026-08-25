#include "Engine/Renderer/Vulkan/renderer_scene_helpers.h"

#include <cmath>
#include <type_traits>
#include <variant>

#include "Engine/Core/Transform.h"

namespace Engine::RendererSceneHelpers {

Particles::ParticleCollider makeParticleCollider(const ColliderComponent& collider,
                                                  const Transform& transform) {
    const Vec3 scale{std::abs(transform.scale.x()), std::abs(transform.scale.y()),
                     std::abs(transform.scale.z())};
    const Vec3 extents = std::visit([]<typename T0>(const T0& shape) {
        using Shape = std::decay_t<T0>;
        if constexpr (std::is_same_v<Shape, BoxCollider>) return shape.halfExtents;
        else if constexpr (std::is_same_v<Shape, SphereCollider>) return Vec3{shape.radius, shape.radius, shape.radius};
        else if constexpr (std::is_same_v<Shape, RampCollider>) return shape.halfExtents;
        else return Vec3{shape.radius, shape.height * 0.5f, shape.radius};
    }, collider.shape) * scale;
    const Vec3 center = transform.position + collider.offset * scale;
    return {Vec4{center.x(), center.y(), center.z(), 0.0f},
            Vec4{extents.x(), extents.y(), extents.z(), 0.0f}};
}

} // namespace Engine::RendererSceneHelpers
