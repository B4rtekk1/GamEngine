#include "Engine/ECS/Actor.h"

#include "Engine/ECS/GameObject.h"
#include "Engine/ECS/Components/RigidbodyRuntime.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>

namespace Engine {
    namespace {
        [[nodiscard]] Vec3 eulerDegrees(const Quat &rotation) {
            const Vec3 axisX = rotation * Vec3{1.0F, 0.0F, 0.0F};
            const Vec3 axisY = rotation * Vec3{0.0F, 1.0F, 0.0F};
            const Vec3 axisZ = rotation * Vec3{0.0F, 0.0F, 1.0F};
            const float y = std::asin(std::clamp(axisZ.x(), -1.0F, 1.0F));
            const float cosY = std::cos(y);
            const float x = std::abs(cosY) > 1.0e-5F
                ? std::atan2(-axisZ.y(), axisZ.z())
                : std::atan2(axisY.z(), axisY.y());
            const float z = std::abs(cosY) > 1.0e-5F
                ? std::atan2(-axisY.x(), axisX.x()) : 0.0F;
            return {x * kDegreesPerRadian, y * kDegreesPerRadian, z * kDegreesPerRadian};
        }

        [[nodiscard]] Transform localTransformFromMatrix(const glm::mat4 &matrix) {
            glm::vec3 scale{}, translation{}, skew{};
            glm::quat rotation{};
            glm::vec4 perspective{};
            if (!glm::decompose(matrix, scale, rotation, translation, skew, perspective)) {
                throw std::runtime_error("Cannot decompose world transform");
            }
            return Transform{
                .position = Vec3{translation},
                .rotation = eulerDegrees(Quat{glm::normalize(rotation)}),
                .scale = Vec3{scale},
            };
        }
    }

    GameObject &Actor::object() const {
        if (scene_ == nullptr) {
            throw std::logic_error("Actor is not attached to a Scene");
        }
        auto *object = scene_->find(objectId_);
        if (object == nullptr) {
            throw std::logic_error("Actor no longer refers to a live object");
        }
        return *object;
    }

    Actor Actor::fromHandle(Scene *scene, const ObjectId objectId) {
        if (scene == nullptr) throw std::logic_error("Component handle is not attached to a Scene");
        return Actor{*scene, objectId};
    }

    bool Actor::valid() const noexcept {
        return scene_ != nullptr && scene_->find(objectId_) != nullptr;
    }

    std::string Actor::name() const { return object().name(); }

    void Actor::setName(std::string name) const {
        if (scene_ != nullptr) {
            scene_->rename(*this, std::move(name));
        }
    }

    bool Actor::hasTag(const std::string_view tag) const {
        const Entity entity = object().entity();
        const Registry &registry = object().registry();
        return registry.has<TagComponent>(entity) && registry.get<TagComponent>(entity).value == tag;
    }

    void Actor::setTag(std::string tag) const {
        if (tag.empty()) {
            throw std::invalid_argument("Actor tag cannot be empty");
        }
        const Entity entity = object().entity();
        Registry &registry = object().registry();
        if (registry.has<TagComponent>(entity)) {
            registry.modify<TagComponent>(entity, [&tag](auto &component) { component.value = std::move(tag); });
            return;
        }
        registry.add<TagComponent>(entity, TagComponent{.value = std::move(tag)});
    }

    void Actor::setPosition(Vec3 position) const { object().setPosition(position); }
    void Actor::setRotation(Vec3 rotation) const { object().setRotation(rotation); }
    void Actor::setScale(Vec3 scale) const { object().setScale(scale); }
    void Actor::translate(const Vec3 offset) const { object().setPosition(object().position() + offset); }
    void Actor::move(const Vec3 offset) const { translate(offset); }
    Vec3 Actor::position() const { return object().position(); }
    Vec3 Actor::rotation() const { return object().rotation(); }
    Vec3 Actor::scale() const { return object().scale(); }
    void Actor::modifyTransform(const std::function<void(Transform &)> &func) const {
        object().modifyTransform(func);
    }

    WorldTransform Actor::worldTransform() const {
        if (scene_ == nullptr) {
            throw std::logic_error("Actor is not attached to a Scene");
        }
        scene_->updateTransforms();
        const Transform &transform = object().transform();
        glm::vec3 ignoredScale{}, ignoredPosition{}, skew{};
        glm::quat rotation{};
        glm::vec4 perspective{};
        if (!glm::decompose(transform.worldMatrix().native(), ignoredScale, rotation, ignoredPosition,
                            skew, perspective)) {
            throw std::runtime_error("Cannot decompose world transform");
        }
        return WorldTransform{
            .position = transform.worldPosition(),
            .rotation = Quat{glm::normalize(rotation)},
            .scale = transform.worldScale(),
        };
    }

    Vec3 Actor::worldPosition() const { return worldTransform().position; }
    Quat Actor::worldRotation() const { return worldTransform().rotation; }
    Vec3 Actor::worldScale() const { return worldTransform().scale; }

    Mat4 Actor::worldMatrix() const {
        if (scene_ == nullptr) {
            throw std::logic_error("Actor is not attached to a Scene");
        }
        scene_->updateTransforms();
        return scene_->worldMatrix(*this);
    }

    void Actor::setWorldPosition(const Vec3 position) const {
        if (scene_ == nullptr) {
            throw std::logic_error("Actor is not attached to a Scene");
        }
        if (hasRigidbody() && object().rigidbody().type == RigidbodyType::Dynamic) {
            teleport(position);
            return;
        }
        scene_->updateTransforms();
        const Entity entity = scene_->findEntity(objectId_);
        Vec3 localPosition = position;
        if (const Actor parentActor = parent(); parentActor.valid()) {
            const glm::mat4 parentWorld = parentActor.worldMatrix().native();
            if (std::abs(glm::determinant(parentWorld)) < 1.0e-6F) {
                throw std::runtime_error("Cannot set world position below a parent with a singular transform");
            }
            localPosition = Vec3{glm::vec3{glm::inverse(parentWorld) * glm::vec4{position.native(), 1.0F}}};
        }
        scene_->registry_.modify<Transform>(entity, [localPosition](Transform &transform) {
            transform.position = localPosition;
        });
    }

    void Actor::setWorldRotation(const Quat rotation) const {
        if (scene_ == nullptr) {
            throw std::logic_error("Actor is not attached to a Scene");
        }
        if (hasRigidbody() && object().rigidbody().type == RigidbodyType::Dynamic) {
            auto& value = object(); if (!value.has<PhysicsCommandBuffer>()) value.add<PhysicsCommandBuffer>();
            value.modify<PhysicsCommandBuffer>([rotation](auto &commands) { commands.teleportRotation = eulerDegrees(rotation.normalized()); });
            return;
        }
        const WorldTransform current = worldTransform();
        Mat4 desiredWorld = Mat4::translate(current.position) * Mat4::rotate(rotation.normalized());
        desiredWorld = Mat4::scale(desiredWorld, current.scale);
        glm::mat4 localMatrix = desiredWorld.native();
        if (const Actor parentActor = parent(); parentActor.valid()) {
            const glm::mat4 parentWorld = parentActor.worldMatrix().native();
            if (std::abs(glm::determinant(parentWorld)) < 1.0e-6F) {
                throw std::runtime_error("Cannot set world rotation below a parent with a singular transform");
            }
            localMatrix = glm::inverse(parentWorld) * localMatrix;
        }
        const Transform local = localTransformFromMatrix(localMatrix);
        const Entity entity = scene_->findEntity(objectId_);
        scene_->registry_.modify<Transform>(entity, [local](Transform &transform) {
            transform.position = local.position;
            transform.rotation = local.rotation;
            transform.scale = local.scale;
        });
    }

    Vec3 Actor::forward() const { return worldRotation() * Vec3{0.0F, 0.0F, -1.0F}; }
    Vec3 Actor::right() const { return worldRotation() * Vec3{1.0F, 0.0F, 0.0F}; }
    Vec3 Actor::up() const { return worldRotation() * Vec3{0.0F, 1.0F, 0.0F}; }

    void Actor::lookAt(const Vec3 target) const {
        const Vec3 direction = target - worldPosition();
        if (direction.length() <= 1.0e-6F) {
            throw std::invalid_argument("Cannot look at the actor's own world position");
        }
        const Vec3 forward = direction.normalized();
        const Vec3 worldUp = std::abs(forward.y()) > 0.999F
            ? Vec3{0.0F, 0.0F, 1.0F} : Vec3{0.0F, 1.0F, 0.0F};
        setWorldRotation(Quat{glm::quatLookAtRH(forward.native(), worldUp.native())});
    }

    Actor Actor::createChild(std::string name) const {
        if (scene_ == nullptr) {
            throw std::logic_error("Actor is not attached to a Scene");
        }
        return scene_->createChild(*this, std::move(name));
    }

    void Actor::setParent(const Actor &parent, const ParentMode mode) const {
        if (scene_ == nullptr) {
            throw std::logic_error("Actor is not attached to a Scene");
        }
        scene_->setParent(*this, parent, mode);
    }

    void Actor::clearParent(const ParentMode mode) const {
        if (scene_ == nullptr) {
            throw std::logic_error("Actor is not attached to a Scene");
        }
        scene_->clearParent(*this, mode);
    }

    Actor Actor::parent() const {
        return scene_ == nullptr ? Actor{} : scene_->parentOf(*this);
    }

    std::vector<Actor> Actor::children() const {
        return scene_ == nullptr ? std::vector<Actor>{} : scene_->childrenOf(*this);
    }

    std::size_t Actor::childCount() const { return children().size(); }
    void Actor::setMesh(std::shared_ptr<const Mesh> mesh) const { object().setMesh(std::move(mesh)); }
    void Actor::setMaterial(const PBRMaterial& material) const { object().setMaterial(material); }
    void Actor::setMaterial(const Material& material) const { object().setMaterial(material); }
    void Actor::setShader(const MaterialShader shader) const { object().setShader(shader); }
    void Actor::setCastShadow(const bool enabled) const { object().setCastShadow(enabled); }
    void Actor::setCullingBatch(const std::uint32_t batch) const { object().setCullingBatch(batch); }
    void Actor::addRigidbody(const RigidbodyComponent& body) const {
        if (body.type == RigidbodyType::Dynamic && parent().valid()) {
            throw std::logic_error("Dynamic rigid bodies cannot be parented; PhysX owns their world pose");
        }
        object().addRigidbody(body);
    }
    bool Actor::hasRigidbody() const { return object().has<RigidbodyComponent>(); }

    void Actor::setBodyType(const RigidbodyType type) const {
        object().modify<RigidbodyComponent>([&](auto &body) { body.type = type; });
    }

    void Actor::setMass(const float mass) const {
        object().modify<RigidbodyComponent>([&](auto &body) { body.mass = mass; });
    }

    void Actor::setGravityEnabled(const bool enabled) const {
        object().modify<RigidbodyComponent>([&](auto &body) { body.useGravity = enabled; });
    }

    void Actor::setVelocity(Vec3 velocity) const {
        auto &value = object();
        if (!value.has<PhysicsCommandBuffer>()) value.add<PhysicsCommandBuffer>();
        value.modify<PhysicsCommandBuffer>([velocity](auto &commands) { commands.linearVelocity = velocity; });
    }

    Vec3 Actor::velocity() const {
        return object().has<RigidbodyState>() ? object().get<RigidbodyState>().linearVelocity : Vec3{};
    }

    void Actor::addRigidbodyForce(const Vec3 value) const {
        auto &object = this->object(); if (!object.has<PhysicsCommandBuffer>()) object.add<PhysicsCommandBuffer>();
        object.modify<PhysicsCommandBuffer>([value](auto &commands) { commands.force += value; });
    }
    void Actor::addRigidbodyImpulse(const Vec3 value) const {
        auto &object = this->object(); if (!object.has<PhysicsCommandBuffer>()) object.add<PhysicsCommandBuffer>();
        object.modify<PhysicsCommandBuffer>([value](auto &commands) { commands.impulse += value; });
    }
    void Actor::teleport(const Vec3 position) const {
        auto &object = this->object(); if (!object.has<PhysicsCommandBuffer>()) object.add<PhysicsCommandBuffer>();
        object.modify<PhysicsCommandBuffer>([position](auto &commands) { commands.teleportPosition = position; });
    }
    void Actor::teleport(const Vec3 position, const Vec3 rotation) const {
        auto &object = this->object(); if (!object.has<PhysicsCommandBuffer>()) object.add<PhysicsCommandBuffer>();
        object.modify<PhysicsCommandBuffer>([position, rotation](auto &commands) { commands.teleportPosition = position; commands.teleportRotation = rotation; });
    }
    float Actor::rigidbodyMass() const { return object().get<RigidbodyComponent>().mass; }

    void Actor::addBoxCollider(Vec3 halfExtents) const {
        auto &object = this->object();
        const ColliderComponent value{.shape = BoxCollider{halfExtents}};
        if (object.has<ColliderComponent>()) { object.modify<ColliderComponent>([&](auto &collider) {
            collider = value;
        });
        } else {
            object.add<ColliderComponent>(value);
        }
    }

    void Actor::addSphereCollider(const float radius) const {
        auto &object = this->object();
        const ColliderComponent value{.shape = SphereCollider{radius}};
        if (object.has<ColliderComponent>()) {
            object.modify<ColliderComponent>([&](auto &collider) { collider = value; });
        } else {
            object.add<ColliderComponent>(value);
        }
    }

    void Actor::addCapsuleCollider(const float radius, const float height) const {
        auto &object = this->object();
        const ColliderComponent value{.shape = CapsuleCollider{radius, height}};
        if (object.has<ColliderComponent>()) {
            object.modify<ColliderComponent>([&](auto &collider) { collider = value; });
        } else { object.add<ColliderComponent>(value);
}
    }

    void Actor::addRampCollider(Vec3 halfExtents) const {
        auto &object = this->object();
        const ColliderComponent value{.shape = RampCollider{halfExtents}};
        if (object.has<ColliderComponent>()) { object.modify<ColliderComponent>([&](auto &collider) {
            collider = value;
        });
        } else { object.add<ColliderComponent>(value);
}
    }

    void Actor::addMeshCollider() const { object().addMeshCollider(); }

    bool Actor::hasCollider() const { return object().has<ColliderComponent>(); }

    void Actor::setColliderTrigger(const bool enabled) const {
        object().modify<ColliderComponent>([&](auto &collider) { collider.isTrigger = enabled; });
    }
    bool Actor::colliderTrigger() const { return object().get<ColliderComponent>().isTrigger; }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): friction and restitution are distinct scalar API inputs.
    void Actor::setColliderMaterial(const float friction, const float restitution) const {
        object().modify<ColliderComponent>([&](auto &collider) {
            collider.friction = friction;
            collider.restitution = restitution;
        });
    }

    void Actor::addCamera(const CameraComponent& camera) const { object().addCamera(camera); }
    bool Actor::hasCamera() const { return object().has<CameraComponent>(); }

    void Actor::setPerspectiveCamera(const float fieldOfView, const float nearClip,
                                     const float farClip) const {
        auto &object = this->object();
        if (!object.has<CameraComponent>()) { object.addCamera();
}
        object.modify<CameraComponent>([&](auto &camera) {
            camera.setPerspective(fieldOfView, nearClip, farClip);
        });
    }

    void Actor::setOrthographicCamera(const float size, const float nearClip,
                                      const float farClip) const {
        auto &object = this->object();
        if (!object.has<CameraComponent>()) { object.addCamera();
}
        object.modify<CameraComponent>([&](auto &camera) {
            camera.setOrthographic(size, nearClip, farClip);
        });
    }

    void Actor::setPrimaryCamera(const bool primary) const {
        object().modify<CameraComponent>([&](auto &camera) { camera.primary = primary; });
    }

    void Actor::setCameraAspectRatio(const float width, const float height) const {
        object().modify<CameraComponent>([&](auto &camera) { camera.setAspectRatio(width, height); });
    }
    void Actor::setCameraFov(const float value) const {
        object().modify<CameraComponent>([value](auto &camera) { camera.setPerspective(value, camera.nearClip, camera.farClip); });
    }
    float Actor::cameraFov() const { return object().get<CameraComponent>().fieldOfView; }

    void Actor::addLight(const LightComponent& light) const { object().addLight(light); }
    bool Actor::hasLight() const { return object().has<LightComponent>(); }

    void Actor::setLightType(const LightType type) const {
        object().modify<LightComponent>([&](auto &light) { light.type = type; });
    }

    void Actor::setLightColor(Math::Color color) const {
        object().modify<LightComponent>([&](auto &light) { light.color = color; });
    }

    void Actor::setLightIntensity(const float intensity) const {
        object().modify<LightComponent>([&](auto &light) { light.intensity = intensity; });
    }
    float Actor::lightIntensity() const { return object().get<LightComponent>().intensity; }

    void Actor::setLightEnabled(const bool enabled) const {
        object().modify<LightComponent>([&](auto &light) { light.enabled = enabled; });
    }

    void Actor::setLightCastShadows(const bool enabled) const {
        object().modify<LightComponent>([&](auto &light) { light.castShadows = enabled; });
    }

    void Actor::addScript(std::string className, const bool enabled) const {
        object().addScript(std::move(className), enabled);
    }

    void Actor::destroy() {
        if (scene_ != nullptr && valid()) {
            if (scene_->deferDestroyDuringScriptUpdate(*this)) return;
            scene_->destroy(*this);
}
        scene_ = nullptr;
        objectId_ = NullObjectId;
    }

    bool TransformHandle::valid() const noexcept { return scene_ != nullptr && scene_->find(objectId_) != nullptr; }
    Vec3 TransformHandle::position() const { return Actor::fromHandle(scene_, objectId_).position(); }
    Vec3 TransformHandle::rotation() const { return Actor::fromHandle(scene_, objectId_).rotation(); }
    Vec3 TransformHandle::scale() const { return Actor::fromHandle(scene_, objectId_).scale(); }
    void TransformHandle::setPosition(const Vec3 value) const { Actor::fromHandle(scene_, objectId_).setPosition(value); }
    void TransformHandle::setRotation(const Vec3 value) const { Actor::fromHandle(scene_, objectId_).setRotation(value); }
    void TransformHandle::setScale(const Vec3 value) const { Actor::fromHandle(scene_, objectId_).setScale(value); }
    void TransformHandle::translate(const Vec3 offset) const { Actor::fromHandle(scene_, objectId_).translate(offset); }

    bool RigidbodyHandle::valid() const noexcept { return scene_ != nullptr && scene_->find(objectId_) != nullptr && Actor::fromHandle(scene_, objectId_).hasRigidbody(); }
    void RigidbodyHandle::setVelocity(const Vec3 value) const { Actor::fromHandle(scene_, objectId_).setVelocity(value); }
    Vec3 RigidbodyHandle::velocity() const { return Actor::fromHandle(scene_, objectId_).velocity(); }
    void RigidbodyHandle::addForce(const Vec3 value) const { Actor::fromHandle(scene_, objectId_).addRigidbodyForce(value); }
    void RigidbodyHandle::addImpulse(const Vec3 value) const { Actor::fromHandle(scene_, objectId_).addRigidbodyImpulse(value); }
    void RigidbodyHandle::teleport(const Vec3 position) const { Actor::fromHandle(scene_, objectId_).teleport(position); }
    void RigidbodyHandle::teleport(const Vec3 position, const Vec3 rotation) const {
        Actor::fromHandle(scene_, objectId_).teleport(position, rotation);
    }
    void RigidbodyHandle::setMass(const float value) const { Actor::fromHandle(scene_, objectId_).setMass(value); }
    float RigidbodyHandle::mass() const { return Actor::fromHandle(scene_, objectId_).rigidbodyMass(); }

    bool ColliderHandle::valid() const noexcept { return scene_ != nullptr && scene_->find(objectId_) != nullptr && Actor::fromHandle(scene_, objectId_).hasCollider(); }
    void ColliderHandle::setTrigger(const bool value) const { Actor::fromHandle(scene_, objectId_).setColliderTrigger(value); }
    bool ColliderHandle::isTrigger() const { return Actor::fromHandle(scene_, objectId_).colliderTrigger(); }

    bool CameraHandle::valid() const noexcept { return scene_ != nullptr && scene_->find(objectId_) != nullptr && Actor::fromHandle(scene_, objectId_).hasCamera(); }
    void CameraHandle::setFov(const float value) const { Actor::fromHandle(scene_, objectId_).setCameraFov(value); }
    float CameraHandle::fov() const { return Actor::fromHandle(scene_, objectId_).cameraFov(); }

    bool LightHandle::valid() const noexcept { return scene_ != nullptr && scene_->find(objectId_) != nullptr && Actor::fromHandle(scene_, objectId_).hasLight(); }
    void LightHandle::setIntensity(const float value) const { Actor::fromHandle(scene_, objectId_).setLightIntensity(value); }
    float LightHandle::intensity() const { return Actor::fromHandle(scene_, objectId_).lightIntensity(); }
} // namespace Engine
