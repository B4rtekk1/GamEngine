#pragma once

// NOLINTBEGIN(readability-magic-numbers)

#include "Engine/Core/Transform.h"
#include "Engine/ECS/Entity.h"
#include "Engine/ECS/Components/RigidbodyComponent.h"
#include "Engine/ECS/Components/ColliderComponent.h"
#include "Engine/ECS/Components/CameraComponent.h"
#include "Engine/Renderer/Geometry/Mesh.h"
#include "Engine/Renderer/Materials/PBRMaterial.h"
#include "Engine/Renderer/Materials/Material.h"
#include "Engine/Scene/Components/LightComponent.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <stdexcept>
#include <vector>

namespace Engine {
    enum class ParentMode {
        KeepWorld,
        KeepLocal,
    };

    class Scene;
    class GameObject;
    class PhysicsSystem;
    class Script;

    class TransformHandle final {
    public:
        [[nodiscard]] bool valid() const noexcept;
        [[nodiscard]] Vec3 position() const;
        [[nodiscard]] Vec3 rotation() const;
        [[nodiscard]] Vec3 scale() const;
        void setPosition(Vec3 value) const;
        void setRotation(Vec3 value) const;
        void setScale(Vec3 value) const;
        void translate(Vec3 offset) const;
    private:
        friend class Actor;
        TransformHandle(Scene *scene, ObjectId id) noexcept : scene_(scene), objectId_(id) {}
        Scene *scene_{}; ObjectId objectId_{NullObjectId};
    };

    class RigidbodyHandle final {
    public:
        [[nodiscard]] bool valid() const noexcept;
        void setVelocity(Vec3 value) const;
        [[nodiscard]] Vec3 velocity() const;
        void addForce(Vec3 value) const;
        void addImpulse(Vec3 value) const;
        /** Queues a world-space teleport for a dynamic rigid body. */
        void teleport(Vec3 position) const;
        /** Queues a world-space teleport, including its Euler rotation in degrees. */
        void teleport(Vec3 position, Vec3 rotation) const;
        void setMass(float value) const;
        [[nodiscard]] float mass() const;
    private:
        friend class Actor;
        RigidbodyHandle(Scene *scene, ObjectId id) noexcept : scene_(scene), objectId_(id) {}
        Scene *scene_{}; ObjectId objectId_{NullObjectId};
    };

    class ColliderHandle final {
    public:
        [[nodiscard]] bool valid() const noexcept;
        void setTrigger(bool value) const;
        [[nodiscard]] bool isTrigger() const;
    private:
        friend class Actor;
        ColliderHandle(Scene *scene, ObjectId id) noexcept : scene_(scene), objectId_(id) {}
        Scene *scene_{}; ObjectId objectId_{NullObjectId};
    };

    class CameraHandle final {
    public:
        [[nodiscard]] bool valid() const noexcept;
        void setFov(float value) const;
        [[nodiscard]] float fov() const;
    private:
        friend class Actor;
        CameraHandle(Scene *scene, ObjectId id) noexcept : scene_(scene), objectId_(id) {}
        Scene *scene_{}; ObjectId objectId_{NullObjectId};
    };

    class LightHandle final {
    public:
        [[nodiscard]] bool valid() const noexcept;
        void setIntensity(float value) const;
        [[nodiscard]] float intensity() const;
    private:
        friend class Actor;
        LightHandle(Scene *scene, ObjectId id) noexcept : scene_(scene), objectId_(id) {}
        Scene *scene_{}; ObjectId objectId_{NullObjectId};
    };

    /** A decomposed, world-space transform exposed by the gameplay API. */
    struct WorldTransform final {
        Vec3 position{};
        Quat rotation{};
        Vec3 scale{1.0F, 1.0F, 1.0F};
    };

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

        /** Returns whether this actor has the supplied gameplay tag. */
        [[nodiscard]] bool hasTag(std::string_view tag) const;

        /** Assigns this actor's gameplay tag. Tags must not be empty. */
        void setTag(std::string tag) const;

        void setPosition(Vec3 position) const;

        void setRotation(Vec3 rotation) const;

        void setScale(Vec3 scale) const;

        void translate(Vec3 offset) const;

        void move(Vec3 offset) const;

        [[nodiscard]] Vec3 position() const;

        [[nodiscard]] Vec3 rotation() const;

        [[nodiscard]] Vec3 scale() const;

        /** High-level handle for this actor's transform. */
        [[nodiscard]] TransformHandle transform() const noexcept { return {scene_, objectId_}; }

        [[nodiscard]] RigidbodyHandle rigidbody() const noexcept { return {scene_, objectId_}; }
        [[nodiscard]] ColliderHandle collider() const noexcept { return {scene_, objectId_}; }
        [[nodiscard]] CameraHandle camera() const noexcept { return {scene_, objectId_}; }
        [[nodiscard]] LightHandle light() const noexcept { return {scene_, objectId_}; }

        /** Mutates the local transform and records a component revision. */
        void modifyTransform(const std::function<void(Transform &)> &func) const;

        /** Returns the resolved transform in world space. */
        [[nodiscard]] WorldTransform worldTransform() const;

        [[nodiscard]] Vec3 worldPosition() const;

        [[nodiscard]] Quat worldRotation() const;

        [[nodiscard]] Vec3 worldScale() const;

        /** Returns the resolved local-to-world matrix. */
        [[nodiscard]] Mat4 worldMatrix() const;

        /** Moves the actor in world space, preserving its local rotation and scale. */
        void setWorldPosition(Vec3 position) const;

        /** Sets the actor's world-space rotation, preserving its world position and scale. */
        void setWorldRotation(Quat rotation) const;

        /** Returns the actor's world-space basis vectors. Forward is local -Z. */
        [[nodiscard]] Vec3 forward() const;
        [[nodiscard]] Vec3 right() const;
        [[nodiscard]] Vec3 up() const;

        /** Rotates the actor so its forward vector faces @p target in world space. */
        void lookAt(Vec3 target) const;

        /** Creates an actor and attaches it as a child of this actor. */
        [[nodiscard]] Actor createChild(std::string name) const;

        /** Attaches this actor to @p parent. Both actors must belong to one scene. */
        void setParent(const Actor &parent, ParentMode mode = ParentMode::KeepWorld) const;

        /** Removes this actor from its current parent, making it a root actor. */
        void clearParent(ParentMode mode = ParentMode::KeepWorld) const;

        /** Returns this actor's parent, or an invalid Actor when it is a root. */
        [[nodiscard]] Actor parent() const;

        /** Returns the current direct children of this actor. */
        [[nodiscard]] std::vector<Actor> children() const;

        [[nodiscard]] std::size_t childCount() const;

        void setMesh(std::shared_ptr<const Mesh> mesh) const;

        void setMaterial(const PBRMaterial &material) const;

        void setMaterial(const Material &material) const;

        void setCastShadow(bool enabled) const;

        void setCullingBatch(std::uint32_t batch) const;

        void addRigidbody(const RigidbodyComponent &body = {}) const;

        [[nodiscard]] bool hasRigidbody() const;

        void setBodyType(RigidbodyType type) const;

        void setMass(float mass) const;

        void setGravityEnabled(bool enabled) const;

        void setVelocity(Vec3 velocity) const;

        [[nodiscard]] Vec3 velocity() const;

        /** Queues a world-space teleport for this dynamic rigid body. */
        void teleport(Vec3 position) const;

        /** Queues a world-space teleport, including Euler rotation in degrees. */
        void teleport(Vec3 position, Vec3 rotation) const;

        void addBoxCollider(Vec3 halfExtents = {0.5F, 0.5F, 0.5F}) const;

        void addSphereCollider(float radius = 0.5F) const;

        void addCapsuleCollider(float radius = 0.5F, float height = 1.0F) const;

        void addRampCollider(Vec3 halfExtents = {0.5F, 0.5F, 0.5F}) const;

        /** Adds an exact triangle collider from the actor's assigned mesh. */
        void addMeshCollider() const;

        [[nodiscard]] bool hasCollider() const;

        void setColliderTrigger(bool enabled) const;

        void setColliderMaterial(float friction, float restitution) const;

        void addCamera(const CameraComponent &camera = {}) const;

        [[nodiscard]] bool hasCamera() const;

        void setPerspectiveCamera(float fieldOfView, float nearClip = 0.1F,
                                  float farClip = 1000.0F) const;

        void setOrthographicCamera(float size, float nearClip = 0.1F,
                                   float farClip = 1000.0F) const;

        void setPrimaryCamera(bool primary) const;

        void setCameraAspectRatio(float width, float height) const;

        void addLight(const LightComponent &light = {}) const;

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
        friend class TransformHandle;
        friend class RigidbodyHandle;
        friend class ColliderHandle;
        friend class CameraHandle;
        friend class LightHandle;

        Actor(Scene &scene, ObjectId objectId) noexcept : scene_(&scene), objectId_(objectId) {
        }

        [[nodiscard]] GameObject &object() const;

        [[nodiscard]] static Actor fromHandle(Scene *scene, ObjectId objectId);
        void addRigidbodyForce(Vec3 value) const;
        void addRigidbodyImpulse(Vec3 value) const;
        [[nodiscard]] float rigidbodyMass() const;
        [[nodiscard]] bool colliderTrigger() const;
        void setCameraFov(float value) const;
        [[nodiscard]] float cameraFov() const;
        [[nodiscard]] float lightIntensity() const;

        Scene *scene_{};
        ObjectId objectId_{NullObjectId};
    };
} // namespace Engine

#include "Engine/Scripting/ScriptRegistry.h"

namespace Engine {
    template<typename T>
    void Actor::attach(const bool enabled) const {
        const auto className = ScriptRegistry::instance().className<T>();
        if (!className) {
            throw std::logic_error("Script type is not registered in the active script module");
        }
        addScript(*className, enabled);
    }

} // namespace Engine

// NOLINTEND(readability-magic-numbers)
