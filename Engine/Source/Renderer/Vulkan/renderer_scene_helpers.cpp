#include "Engine/Renderer/Vulkan/renderer_scene_helpers.h"

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <utility>
#include <variant>

#include "Engine/Core/Transform.h"
#include "Engine/Renderer/Geometry/Mesh.h"

namespace Engine::RendererSceneHelpers {
    namespace {
        constexpr float HalfHeightFactor = 0.5F;

        std::pair<Vec3, Vec3> meshBounds(const MeshCollider& collider) {
            if (collider.mesh == nullptr || collider.mesh->vertices.empty()) return {};
            Vec3 minimum = collider.mesh->vertices.front().position;
            Vec3 maximum = minimum;
            for (const Vertex& vertex : collider.mesh->vertices) {
                const Vec3 position = vertex.position;
                minimum = {std::min(minimum.x(), position.x()), std::min(minimum.y(), position.y()),
                           std::min(minimum.z(), position.z())};
                maximum = {std::max(maximum.x(), position.x()), std::max(maximum.y(), position.y()),
                           std::max(maximum.z(), position.z())};
            }
            return {(minimum + maximum) * HalfHeightFactor,
                    (maximum - minimum) * HalfHeightFactor};
        }
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
            } else if constexpr (std::is_same_v<Shape, MeshCollider>) {
                // Particle simulation supports AABBs only; use the mesh's
                // local bounds as its conservative approximation.
                return meshBounds(shape).second;
            } else {
                return Vec3{shape.radius, shape.height * HalfHeightFactor, shape.radius};
            }
        }, collider.shape) * scale;
        const Vec3 meshCenter = std::holds_alternative<MeshCollider>(collider.shape)
            ? meshBounds(std::get<MeshCollider>(collider.shape)).first
            : Vec3{};
        const Vec3 center = transform.position + (collider.offset + meshCenter) * scale;
        return {
            Vec4{center.x(), center.y(), center.z(), 0.0F},
            Vec4{extents.x(), extents.y(), extents.z(), 0.0F},
        };
    }
} // namespace Engine::RendererSceneHelpers
