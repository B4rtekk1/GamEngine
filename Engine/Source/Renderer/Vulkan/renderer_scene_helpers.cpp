#include "Engine/Renderer/Vulkan/renderer_scene_helpers.h"

#include <cmath>
#include <type_traits>
#include <variant>

#include "Engine/Core/Transform.h"

namespace Engine::RendererSceneHelpers {
    namespace {
        constexpr float HalfHeightFactor = 0.5F;
    }

    Particles::ParticleCollider makeParticleCollider(const ColliderComponent &collider,
                                                     const Transform &transform) {
        const Vec3 scale{
            std::abs(transform.scale.x()), std::abs(transform.scale.y()),
            std::abs(transform.scale.z())
        };
        const Vec3 extents = std::visit([]<typename T0>(const T0 &shape) {
            using Shape = std::decay_t<T0>;
            if constexpr (std::is_same_v<Shape, BoxCollider> || std::is_same_v<Shape, RampCollider>) {
                return shape.halfExtents;
            } else if constexpr (std::is_same_v<Shape, SphereCollider>) {
                return Vec3{shape.radius, shape.radius, shape.radius};
            } else {
                return Vec3{shape.radius, shape.height * HalfHeightFactor, shape.radius};
            }
        }, collider.shape) * scale;
        const Vec3 center = transform.position + collider.offset * scale;
        return {
            Vec4{center.x(), center.y(), center.z(), 0.0F},
            Vec4{extents.x(), extents.y(), extents.z(), 0.0F},
        };
    }
} // namespace Engine::RendererSceneHelpers
