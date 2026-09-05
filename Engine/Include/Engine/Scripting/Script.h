#pragma once

#include "Engine/Core/Transform.h"
#include "Engine/ECS/Actor.h"
#include "Engine/ECS/Entity.h"
#include "Engine/ECS/Registry.h"

#include <vector>

namespace Engine {
    class Scene;

    /** Base class for native C++ behaviours attached to entities. */
    class Script {
    public:
        virtual ~Script() = default;

        [[nodiscard]] Entity entity() const noexcept { return entity_; }
        [[nodiscard]] Registry &registry() const noexcept { return *registry_; }
        /** Returns the script entity's read-only local transform. */
        [[nodiscard]] const Transform &transform() const { return registry().get<Transform>(entity_); }

        void setPosition(Vec3 value) { registry().modify<Transform>(entity_, [&](auto &transform) {
            transform.position = value;
        }); }
        void setRotation(Vec3 value) { registry().modify<Transform>(entity_, [&](auto &transform) {
            transform.rotation = value;
        }); }
        void setScale(Vec3 value) { registry().modify<Transform>(entity_, [&](auto &transform) {
            transform.scale = value;
        }); }
        void translate(Vec3 offset) { registry().modify<Transform>(entity_, [&](auto &transform) {
            transform.position += offset;
        }); }
        void rotate(Vec3 offset) { registry().modify<Transform>(entity_, [&](auto &transform) {
            transform.rotation += offset;
        }); }

        /** Returns the high-level actor controlled by this script. */
        [[nodiscard]] Actor actor() const;

        /** Returns this script actor's parent, or an invalid Actor for a root actor. */
        [[nodiscard]] Actor parent() const;

        /** Returns this script actor's direct children. */
        [[nodiscard]] std::vector<Actor> children() const;

        /** Returns the scene containing the scripted actor. */
        [[nodiscard]] Scene &scene() const;

        /** Called once, immediately before the first update. */
        virtual void onCreate() {
        }

        /** Called after creation, and whenever the ScriptComponent becomes enabled. */
        virtual void onEnable() {
        }

        /** Called once per frame while the ScriptComponent is enabled. */
        virtual void onUpdate(float deltaTime) { (void) deltaTime; }

        /** Called before a live ScriptComponent is disabled or destroyed. */
        virtual void onDisable() {
        }

        /** Called when the runtime instance is removed. */
        virtual void onDestroy() {
        }

    private:
        friend class ScriptSystem;

        void attach(Registry &registry, Entity entity) noexcept {
            scene_ = nullptr;
            registry_ = &registry;
            entity_ = entity;
        }

        void attach(Scene &scene, Registry &registry, Entity entity) noexcept {
            scene_ = &scene;
            registry_ = &registry;
            entity_ = entity;
        }

        Scene *scene_{};
        Registry *registry_{};
        Entity entity_{NullEntity};
    };
} // namespace Engine
