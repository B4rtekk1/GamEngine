#include "Engine/ECS/Actor.h"

#include "Engine/ECS/GameObject.h"
#include "Engine/Scene/Scene.h"

#include <stdexcept>
#include <utility>

namespace Engine {

GameObject& Actor::object() const {
    if (scene_ == nullptr) throw std::logic_error("Actor is not attached to a Scene");
    auto* object = scene_->find(objectId_);
    if (object == nullptr) throw std::logic_error("Actor no longer refers to a live object");
    return *object;
}

bool Actor::valid() const noexcept {
    return scene_ != nullptr && scene_->find(objectId_) != nullptr;
}

std::string Actor::name() const { return object().name(); }

void Actor::setName(std::string name) {
    if (scene_ != nullptr) scene_->rename(*this, std::move(name));
}
void Actor::setPosition(Vec3 position) { object().setPosition(position); }
void Actor::setRotation(Vec3 rotation) { object().setRotation(rotation); }
void Actor::setScale(Vec3 scale) { object().setScale(scale); }
void Actor::translate(const Vec3 offset) { object().setPosition(object().position() + offset); }
void Actor::move(const Vec3 offset) { translate(offset); }
Vec3 Actor::position() const { return object().position(); }
Vec3 Actor::rotation() const { return object().rotation(); }
Vec3 Actor::scale() const { return object().scale(); }
void Actor::setMesh(std::shared_ptr<const Mesh> mesh) { object().setMesh(std::move(mesh)); }
void Actor::setMaterial(PBRMaterial material) { object().setMaterial(std::move(material)); }
void Actor::setCastShadow(const bool enabled) { object().setCastShadow(enabled); }
void Actor::setCullingBatch(const std::uint32_t batch) { object().setCullingBatch(batch); }
void Actor::addRigidbody(RigidbodyComponent body) { object().addRigidbody(std::move(body)); }
bool Actor::hasRigidbody() const { return object().has<RigidbodyComponent>(); }
void Actor::setBodyType(const RigidbodyType type) {
    object().modify<RigidbodyComponent>([&](auto& body) { body.type = type; });
}
void Actor::setMass(const float mass) {
    object().modify<RigidbodyComponent>([&](auto& body) { body.mass = mass; });
}
void Actor::setGravityEnabled(const bool enabled) {
    object().modify<RigidbodyComponent>([&](auto& body) { body.useGravity = enabled; });
}
void Actor::setVelocity(Vec3 velocity) {
    object().modify<RigidbodyComponent>([&](auto& body) { body.linearVelocity = velocity; });
}
Vec3 Actor::velocity() const {
    return object().get<RigidbodyComponent>().linearVelocity;
}

void Actor::addBoxCollider(Vec3 halfExtents) {
    auto& object = this->object();
    const ColliderComponent value{.shape = BoxCollider{halfExtents}};
    if (object.has<ColliderComponent>()) object.modify<ColliderComponent>([&](auto& collider) { collider = value; });
    else object.add<ColliderComponent>(value);
}
void Actor::addSphereCollider(const float radius) {
    auto& object = this->object();
    const ColliderComponent value{.shape = SphereCollider{radius}};
    if (object.has<ColliderComponent>()) object.modify<ColliderComponent>([&](auto& collider) { collider = value; });
    else object.add<ColliderComponent>(value);
}
void Actor::addCapsuleCollider(const float radius, const float height) {
    auto& object = this->object();
    const ColliderComponent value{.shape = CapsuleCollider{radius, height}};
    if (object.has<ColliderComponent>()) object.modify<ColliderComponent>([&](auto& collider) { collider = value; });
    else object.add<ColliderComponent>(value);
}
void Actor::addRampCollider(Vec3 halfExtents) {
    auto& object = this->object();
    const ColliderComponent value{.shape = RampCollider{halfExtents}};
    if (object.has<ColliderComponent>()) object.modify<ColliderComponent>([&](auto& collider) { collider = value; });
    else object.add<ColliderComponent>(value);
}
bool Actor::hasCollider() const { return object().has<ColliderComponent>(); }
void Actor::setColliderTrigger(const bool enabled) {
    object().modify<ColliderComponent>([&](auto& collider) { collider.isTrigger = enabled; });
}
void Actor::setColliderMaterial(const float friction, const float restitution) {
    object().modify<ColliderComponent>([&](auto& collider) {
        collider.friction = friction;
        collider.restitution = restitution;
    });
}

void Actor::addCamera(CameraComponent camera) { object().addCamera(std::move(camera)); }
bool Actor::hasCamera() const { return object().has<CameraComponent>(); }
void Actor::setPerspectiveCamera(const float fieldOfView, const float nearClip,
                                 const float farClip) {
    auto& object = this->object();
    if (!object.has<CameraComponent>()) object.addCamera();
    object.modify<CameraComponent>([&](auto& camera) {
        camera.setPerspective(fieldOfView, nearClip, farClip);
    });
}
void Actor::setOrthographicCamera(const float size, const float nearClip,
                                  const float farClip) {
    auto& object = this->object();
    if (!object.has<CameraComponent>()) object.addCamera();
    object.modify<CameraComponent>([&](auto& camera) {
        camera.setOrthographic(size, nearClip, farClip);
    });
}
void Actor::setPrimaryCamera(const bool primary) {
    object().modify<CameraComponent>([&](auto& camera) { camera.primary = primary; });
}
void Actor::setCameraAspectRatio(const float width, const float height) {
    object().modify<CameraComponent>([&](auto& camera) { camera.setAspectRatio(width, height); });
}

void Actor::addLight(LightComponent light) { object().addLight(std::move(light)); }
bool Actor::hasLight() const { return object().has<LightComponent>(); }
void Actor::setLightType(const LightType type) {
    object().modify<LightComponent>([&](auto& light) { light.type = type; });
}
void Actor::setLightColor(Math::Color color) {
    object().modify<LightComponent>([&](auto& light) { light.color = color; });
}
void Actor::setLightIntensity(const float intensity) {
    object().modify<LightComponent>([&](auto& light) { light.intensity = intensity; });
}
void Actor::setLightEnabled(const bool enabled) {
    object().modify<LightComponent>([&](auto& light) { light.enabled = enabled; });
}
void Actor::setLightCastShadows(const bool enabled) {
    object().modify<LightComponent>([&](auto& light) { light.castShadows = enabled; });
}

void Actor::addScript(std::string className, const bool enabled) {
    object().addScript(std::move(className), enabled);
}
void Actor::destroy() {
    if (scene_ != nullptr && valid()) scene_->destroy(*this);
    scene_ = nullptr;
    objectId_ = NullObjectId;
}

} // namespace Engine
