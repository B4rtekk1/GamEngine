#include "Engine/ECS/Actor.h"

#include "Engine/ECS/GameObject.h"
#include "Engine/Scene/Scene.h"

#include <stdexcept>
#include <utility>

namespace Engine {
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

    bool Actor::valid() const noexcept {
        return scene_ != nullptr && scene_->find(objectId_) != nullptr;
    }

    std::string Actor::name() const { return object().name(); }

    void Actor::setName(std::string name) const {
        if (scene_ != nullptr) {
            scene_->rename(*this, std::move(name));
        }
    }

    void Actor::setPosition(Vec3 position) const { object().setPosition(position); }
    void Actor::setRotation(Vec3 rotation) const { object().setRotation(rotation); }
    void Actor::setScale(Vec3 scale) const { object().setScale(scale); }
    void Actor::translate(const Vec3 offset) const { object().setPosition(object().position() + offset); }
    void Actor::move(const Vec3 offset) const { translate(offset); }
    Vec3 Actor::position() const { return object().position(); }
    Vec3 Actor::rotation() const { return object().rotation(); }
    Vec3 Actor::scale() const { return object().scale(); }
    void Actor::setMesh(std::shared_ptr<const Mesh> mesh) const { object().setMesh(std::move(mesh)); }
    void Actor::setMaterial(const PBRMaterial& material) const { object().setMaterial(material); }
    void Actor::setCastShadow(const bool enabled) const { object().setCastShadow(enabled); }
    void Actor::setCullingBatch(const std::uint32_t batch) const { object().setCullingBatch(batch); }
    void Actor::addRigidbody(const RigidbodyComponent& body) const { object().addRigidbody(body); }
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
        object().modify<RigidbodyComponent>([&](auto &body) { body.linearVelocity = velocity; });
    }

    Vec3 Actor::velocity() const {
        return object().get<RigidbodyComponent>().linearVelocity;
    }

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
        if (scene_ != nullptr && valid()) { scene_->destroy(*this);
}
        scene_ = nullptr;
        objectId_ = NullObjectId;
    }
} // namespace Engine
