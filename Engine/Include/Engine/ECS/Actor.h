#pragma once

#include "Engine/Core/Transform.h"
#include "Engine/ECS/Entity.h"
#include "Engine/ECS/Components/RigidbodyComponent.h"
#include "Engine/ECS/Components/ColliderComponent.h"
#include "Engine/ECS/Components/CameraComponent.h"
#include "Engine/Renderer/Geometry/Mesh.h"
#include "Engine/Renderer/Materials/PBRMaterial.h"
#include "Engine/Scene/Components/LightComponent.h"

#include <cstdint>
#include <memory>
#include <string>
#include <stdexcept>

namespace Engine {

class Scene;
class GameObject;
class PhysicsSystem;
class Script;

/**
 * High-level, non-owning handle to an object in a Scene.
 *
 * Actor intentionally does not expose Registry or generic component access.
 * It is safe to copy and is resolved through the scene's stable ObjectId.
 */
class Actor final {
public:
    Actor() = default;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] ObjectId id() const noexcept { return objectId_; }
    [[nodiscard]] std::string name() const;

    void setName(std::string name) const;
    void setPosition(Vec3 position) const;
    void setRotation(Vec3 rotation) const;
    void setScale(Vec3 scale) const;
    void translate(Vec3 offset) const;
    void move(Vec3 offset) const;
    [[nodiscard]] Vec3 position() const;
    [[nodiscard]] Vec3 rotation() const;
    [[nodiscard]] Vec3 scale() const;

    void setMesh(std::shared_ptr<const Mesh> mesh) const;
    void setMaterial(const PBRMaterial& material) const;
    void setCastShadow(bool enabled) const;
    void setCullingBatch(std::uint32_t batch) const;

    void addRigidbody(const RigidbodyComponent& body = {}) const;
    [[nodiscard]] bool hasRigidbody() const;
    void setBodyType(RigidbodyType type) const;
    void setMass(float mass) const;
    void setGravityEnabled(bool enabled) const;
    void setVelocity(Vec3 velocity) const;
    [[nodiscard]] Vec3 velocity() const;

    void addBoxCollider(Vec3 halfExtents = {0.5f, 0.5f, 0.5f}) const;
    void addSphereCollider(float radius = 0.5f) const;
    void addCapsuleCollider(float radius = 0.5f, float height = 1.0f) const;
    void addRampCollider(Vec3 halfExtents = {0.5f, 0.5f, 0.5f}) const;
    [[nodiscard]] bool hasCollider() const;
    void setColliderTrigger(bool enabled) const;
    void setColliderMaterial(float friction, float restitution) const;

    void addCamera(const CameraComponent& camera = {}) const;
    [[nodiscard]] bool hasCamera() const;
    void setPerspectiveCamera(float fieldOfView, float nearClip = 0.1f,
                              float farClip = 1000.0f) const;
    void setOrthographicCamera(float size, float nearClip = 0.1f,
                               float farClip = 1000.0f) const;
    void setPrimaryCamera(bool primary) const;
    void setCameraAspectRatio(float width, float height) const;

    void addLight(const LightComponent& light = {}) const;
    [[nodiscard]] bool hasLight() const;
    void setLightType(LightType type) const;
    void setLightColor(Math::Color color) const;
    void setLightIntensity(float intensity) const;
    void setLightEnabled(bool enabled) const;
    void setLightCastShadows(bool enabled) const;

    void addScript(std::string className, bool enabled = true) const;

    template<typename T>
    void attach(bool enabled = true) const;

    void destroy();

private:
    friend class Scene;
    friend class PhysicsSystem;
    friend class Script;
    Actor(Scene& scene, ObjectId objectId) noexcept : scene_(&scene), objectId_(objectId) {}

    [[nodiscard]] GameObject& object() const;

    Scene* scene_{};
    ObjectId objectId_{NullObjectId};
};

} // namespace Engine

#include "Engine/Scripting/ScriptRegistry.h"

namespace Engine {

template<typename T>
void Actor::attach(const bool enabled) const {
    const auto className = ScriptRegistry::instance().className<T>();
    if (!className) {
        throw std::logic_error("Script type is not registered; use ENGINE_REGISTER_SCRIPT first");
    }
    addScript(*className, enabled);
}

} // namespace Engine
