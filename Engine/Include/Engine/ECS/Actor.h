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

    void setName(std::string name);
    void setPosition(Vec3 position);
    void setRotation(Vec3 rotation);
    void setScale(Vec3 scale);
    void translate(Vec3 offset);
    void move(Vec3 offset);
    [[nodiscard]] Vec3 position() const;
    [[nodiscard]] Vec3 rotation() const;
    [[nodiscard]] Vec3 scale() const;

    void setMesh(std::shared_ptr<const Mesh> mesh);
    void setMaterial(PBRMaterial material);
    void setCastShadow(bool enabled);
    void setCullingBatch(std::uint32_t batch);

    void addRigidbody(RigidbodyComponent body = {});
    void addBoxCollider(Vec3 halfExtents = {0.5f, 0.5f, 0.5f});
    void addSphereCollider(float radius = 0.5f);
    void addCapsuleCollider(float radius = 0.5f, float height = 1.0f);
    void addCamera(CameraComponent camera = {});
    void addLight(LightComponent light = {});
    void addScript(std::string className, bool enabled = true);

    template<typename T>
    void attach(bool enabled = true);

    void destroy();

private:
    friend class Scene;
    friend class PhysicsSystem;
    friend class Script;
    Actor(Scene& scene, ObjectId objectId) noexcept : scene_(&scene), objectId_(objectId) {}

    GameObject& object() const;

    Scene* scene_{};
    ObjectId objectId_{NullObjectId};
};

} // namespace Engine

#include "Engine/Scripting/ScriptRegistry.h"

namespace Engine {

template<typename T>
void Actor::attach(const bool enabled) {
    const auto className = ScriptRegistry::instance().className<T>();
    if (!className) {
        throw std::logic_error("Script type is not registered; use ENGINE_REGISTER_SCRIPT first");
    }
    addScript(*className, enabled);
}

} // namespace Engine
