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

void Actor::setName(std::string name) { object().setName(std::move(name)); }
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
void Actor::addBoxCollider(Vec3 halfExtents) {
    object().add<ColliderComponent>(ColliderComponent{.shape = BoxCollider{halfExtents}});
}
void Actor::addSphereCollider(const float radius) {
    object().add<ColliderComponent>(ColliderComponent{.shape = SphereCollider{radius}});
}
void Actor::addCapsuleCollider(const float radius, const float height) {
    object().add<ColliderComponent>(ColliderComponent{.shape = CapsuleCollider{radius, height}});
}
void Actor::addCamera(CameraComponent camera) { object().addCamera(std::move(camera)); }
void Actor::addLight(LightComponent light) { object().addLight(std::move(light)); }
void Actor::addScript(std::string className, const bool enabled) {
    object().addScript(std::move(className), enabled);
}
void Actor::destroy() {
    if (scene_ != nullptr && valid()) scene_->destroy(*this);
    scene_ = nullptr;
    objectId_ = NullObjectId;
}

} // namespace Engine
