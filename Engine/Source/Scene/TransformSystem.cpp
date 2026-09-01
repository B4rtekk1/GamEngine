#include "Engine/Scene/TransformSystem.h"

#include "Engine/Scene/Components/IdentityComponents.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>

#include <unordered_map>

namespace Engine {
    namespace {
        [[nodiscard]] bool same(const Vec3 &left, const Vec3 &right) noexcept {
            return left.x() == right.x() && left.y() == right.y() && left.z() == right.z();
        }

        [[nodiscard]] Entity parentEntity(const Registry &registry,
                                          const std::unordered_map<UUID, Entity> &byUuid,
                                          const Entity entity) {
            if (!registry.has<ParentComponent>(entity)) return NullEntity;
            const auto parent = byUuid.find(registry.get<ParentComponent>(entity).parent);
            return parent == byUuid.end() ? NullEntity : parent->second;
        }
    }

    void TransformSystem::update(Registry &registry) {
        std::unordered_map<UUID, Entity> byUuid;
        byUuid.reserve(registry.size());
        registry.view<UUIDComponent>([&](const Entity entity, const UUIDComponent &uuid) {
            byUuid.emplace(uuid.value, entity);
        });

        std::unordered_map<Entity, std::uint8_t> state;
        state.reserve(registry.size());
        const auto resolve = [&](auto &&self, const Entity entity) -> void {
            if (!registry.valid(entity) || !registry.has<Transform>(entity)) return;
            std::uint8_t &visit = state[entity];
            if (visit == 2) return;
            if (visit == 1) return; // Invalid cycles are handled as roots defensively.
            visit = 1;

            Transform &transform = registry.get<Transform>(entity);
            const Entity parent = parentEntity(registry, byUuid, entity);
            if (parent != NullEntity && parent != entity && state[parent] != 1) self(self, parent);

            const Transform *parentTransform = parent != NullEntity && registry.has<Transform>(parent)
                                                   ? &registry.get<Transform>(parent) : nullptr;
            const std::uint64_t parentRevision = parentTransform == nullptr ? 0 : parentTransform->worldRevision();
            const bool localChanged = !same(transform.position, transform.cachedLocalPosition) ||
                                      !same(transform.rotation, transform.cachedLocalRotation) ||
                                      !same(transform.scale, transform.cachedLocalScale);
            const bool changed = !transform.worldCacheValid || localChanged ||
                                 transform.cachedParent != parent ||
                                 transform.cachedParentWorldRevision != parentRevision;
            if (changed) {
                transform.previousCachedWorldMatrix = transform.cachedWorldMatrix;
                transform.cachedWorldMatrix = parentTransform == nullptr
                    ? transform.matrix()
                    : Mat4{parentTransform->worldMatrix().native() * transform.matrix().native()};
                transform.cachedLocalPosition = transform.position;
                transform.cachedLocalRotation = transform.rotation;
                transform.cachedLocalScale = transform.scale;
                transform.cachedParent = parent;
                transform.cachedParentWorldRevision = parentRevision;
                ++transform.cachedWorldRevision;
                transform.worldCacheValid = true;
            }
            visit = 2;
        };
        registry.view<Transform>([&](const Entity entity, Transform &) { resolve(resolve, entity); });
    }

    const Mat4 &TransformSystem::worldMatrix(Registry &registry, const Entity entity) {
        update(registry);
        return registry.get<Transform>(entity).worldMatrix();
    }

    Transform TransformSystem::worldTransform(Registry &registry, const Entity entity) {
        const glm::mat4 matrix = worldMatrix(registry, entity).native();
        glm::vec3 scale{};
        glm::quat rotation{};
        glm::vec3 translation{};
        glm::vec3 skew{};
        glm::vec4 perspective{};
        if (!glm::decompose(matrix, scale, rotation, translation, skew, perspective)) {
            return registry.get<Transform>(entity);
        }
        return Transform{.position = Vec3{translation},
                         .rotation = Vec3{glm::degrees(glm::eulerAngles(glm::conjugate(rotation)))},
                         .scale = Vec3{scale}};
    }
} // namespace Engine
