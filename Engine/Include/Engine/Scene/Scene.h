#pragma once

#include "Engine/ECS/GameObject.h"
#include "Engine/ECS/Actor.h"
#include "Engine/ECS/Registry.h"
#include "Engine/Renderer/Particles/ParticleSystem.h"
#include "Engine/ECS/Components/ParticleEmitterComponent.h"
#include "Engine/ECS/Components/SmokeEmitterComponent.h"
#include "Engine/ECS/Components/ColorPickerComponent.h"
#include "Engine/UI/Canvas.h"
#include "Engine/UI/Vulkan/UIFontAtlas.h"
#include "Engine/Scene/Components/IdentityComponents.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Scene/Prefab.h"
#include "Engine/UI/Interface.h"
#include "Engine/ECS/Components/CameraComponent.h"
#include "Engine/ECS/Components/TerrainComponent.h"
#include "Engine/Scene/Components/LightComponent.h"
#include "Engine/Renderer/Geometry/Mesh.h"
#include "Engine/Renderer/Materials/PBRMaterial.h"

#include <memory>
#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace Engine {
    class SceneSerializer;
}

namespace Engine::Assets {
    class Content;
}

namespace Engine {
    class Renderer;
    class ScriptSystem;
    class PhysicsSystem;
    class SceneEditor;
    class Application;

    // Runtime scene data. Content creation belongs to ScenePresets (or to the
    // application), rather than to this data container.
    class Scene {
    public:
        /**
         * Advanced component-oriented creation API.
         *
         * Prefer createActor() for gameplay code. This overload is retained
         * for engine and editor code that needs a GameObject's component API.
         */
        [[nodiscard]] GameObject &create(std::string name) {
            if (name.empty()) {
                throw std::invalid_argument("Scene object name cannot be empty");
            }
            const std::string baseName = name;
            std::size_t suffix = 2;
            while (find(name) != nullptr) {
                name = baseName + " " + std::to_string(suffix++);
            }

            auto object = std::unique_ptr<GameObject>(new GameObject(registry_, name));
            object->spawn();
            GameObject &result = *object;
            registry_.add<NameComponent>(result.entity(), NameComponent{.value = name});
            registry_.add<UUIDComponent>(result.entity(), UUIDComponent{.value = createUUID()});
            names_[result.name()] = result.objectId();
            objects_.push_back(std::move(object));
            return result;
        }

        /**
         * Advanced component-oriented creation API.
         *
         * New game code should prefer createActor() and the other Actor-returning
         * factories below.  This remains available for editor and engine code
         * that needs direct component access.
         */
        [[nodiscard]] GameObject &createGameObject(std::string name) {
            return create(std::move(name));
        }

        /** High-level actor creation without exposing the ECS entity handle. */
        [[nodiscard]] Actor createActor(std::string name) {
            auto &object = create(std::move(name));
            return Actor{*this, object.objectId()};
        }

        /** Creates a new actor and immediately attaches it below @p parent. */
        [[nodiscard]] Actor createChild(const Actor &parent, std::string name) {
            if (parent.scene_ != this || !parent.valid()) {
                throw std::invalid_argument("Parent actor must belong to this Scene");
            }
            Actor child = createActor(std::move(name));
            setParent(child, parent, ParentMode::KeepLocal);
            return child;
        }

        /** Duplicates an actor without exposing its ECS entity identifier. */
        [[nodiscard]] Actor duplicate(const Actor &actor) {
            if (actor.scene_ != this || !actor.valid()) {
                return {};
            }
            auto &object = duplicate(actor.object().entity());
            return Actor{*this, object.objectId()};
        }

        /** Creates a copy of an existing object with a fresh name and UUID. */
        [[nodiscard]] GameObject &duplicate(const Entity entity) {
            GameObject *source = findByEntity(entity);
            if (source == nullptr) {
                throw std::out_of_range("Scene entity is not a GameObject");
            }

            std::string name = source->name();
            if (registry_.has<NameComponent>(entity)) {
                name = registry_.get<NameComponent>(entity).value;
            }
            const std::string baseName = name;
            std::size_t suffix = 2;
            while (find(name) != nullptr) {
                name = baseName + " " + std::to_string(suffix++);
            }

            auto object = std::make_unique<GameObject>(source->clone());
            object->setName(name);
            const Entity copiedEntity = object->entity();
            registry_.modify<NameComponent>(copiedEntity, [&](auto &value) { value.value = name; });
            registry_.modify<UUIDComponent>(copiedEntity, [&](auto &value) { value.value = createUUID(); });
            names_[name] = object->objectId();
            objects_.push_back(std::move(object));
            return *objects_.back();
        }

        /** Creates a renderable actor from an already-loaded mesh. */
        [[nodiscard]] Actor createMesh(std::string name,
                                       std::shared_ptr<const Mesh> mesh,
                                       PBRMaterial material = {});

        /** Loads a model through Content and creates an actor in one operation. */
        [[nodiscard]] Actor createModel(std::string name,
                                        std::filesystem::path path,
                                        const Assets::Content &content);

        /** Loads a model using the Content service attached to this scene. */
        [[nodiscard]] Actor createModel(std::string name, std::filesystem::path path);

        /** Loads a model prefab and instantiates it in one operation. */
        [[nodiscard]] Actor createPrefab(std::string name,
                                         std::filesystem::path path,
                                         PBRMaterial material = {});

        [[nodiscard]] Actor createCube(std::string name, const PBRMaterial &material = {});

        [[nodiscard]] Actor createPrefab(std::string name, const Prefab &prefab);

        /** Creates a checkerboard terrain backed by an editable heightmap. */
        [[nodiscard]] Actor createTerrain(std::string name, TerrainComponent terrain = {});

        [[nodiscard]] Actor findActor(const std::string &name) noexcept {
            auto *object = find(name);
            return object == nullptr ? Actor{} : Actor{*this, object->objectId()};
        }

        /** Creates a camera actor. */
        [[nodiscard]] Actor createCamera(std::string name, const CameraComponent &camera = {}) {
            auto &object = create(std::move(name));
            object.addCamera(camera);
            return Actor{*this, object.objectId()};
        }

        /** Creates a light actor. */
        [[nodiscard]] Actor createLight(std::string name, const LightComponent &light = {}) {
            auto &object = create(std::move(name));
            object.addLight(light);
            if (light.type == LightType::Directional && light.enabled) {
                setActiveDirectionalLight(object.entity());
            }
            return Actor{*this, object.objectId()};
        }

        /** Compatibility spelling; use createCamera(). */
        [[nodiscard]] Actor createCameraActor(std::string name, const CameraComponent &camera = {}) {
            return createCamera(std::move(name), camera);
        }

        /** Compatibility spelling; use createLight(). */
        [[nodiscard]] Actor createLightActor(std::string name, const LightComponent &light = {}) {
            return createLight(std::move(name), light);
        }

        /** Renames an actor while keeping name lookup consistent. */
        void rename(const Actor &actor, std::string name) {
            if (actor.scene_ != this || !actor.valid()) {
                return;
            }
            rename(actor.object().entity(), std::move(name));
        }

        /** Renames an editor entity while keeping object and name lookup state consistent. */
        void rename(const Entity entity, std::string name) {
            auto *object = findByEntity(entity);
            if (object == nullptr) {
                throw std::out_of_range("Scene entity is not a GameObject");
            }
            if (name.empty()) {
                throw std::invalid_argument("Scene object name cannot be empty");
            }
            if (auto *existing = find(name); existing != nullptr && existing != object) {
                throw std::invalid_argument("Scene object name is already in use: " + name);
            }
            names_.erase(object->name());
            object->setName(std::move(name));
            registry_.modify<NameComponent>(object->entity(), [&](auto &component) {
                component.value = object->name();
            });
            names_[object->name()] = object->objectId();
        }

        /** Destroys an actor through the high-level runtime API. */
        void destroy(const Actor &actor) {
            if (actor.scene_ != this || actor.objectId_ == NullObjectId) {
                return;
            }
            if (auto *object = find(actor.objectId_)) {
                destroy(object->entity());
            }
        }

        /** Attaches @p child below @p parent, rejecting self-parenting and cycles. */
        void setParent(const Actor &child, const Actor &parent,
                       ParentMode mode = ParentMode::KeepWorld);

        /** Removes @p child's parent link. */
        void clearParent(const Actor &child, ParentMode mode = ParentMode::KeepWorld);

        [[nodiscard]] Actor parentOf(const Actor &child) const noexcept;

        [[nodiscard]] std::vector<Actor> childrenOf(const Actor &parent) const;

        /** Resolves local transforms and returns an actor's cached world matrix. */
        [[nodiscard]] const Mat4 &worldMatrix(const Actor &actor) {
            if (actor.scene_ != this || !actor.valid()) {
                throw std::invalid_argument("Actor must be a live actor in this Scene");
            }
            return TransformSystem::worldMatrix(registry_, findEntity(actor.objectId_));
        }

        /** Updates the hierarchy's cached world transforms. */
        void updateTransforms() { TransformSystem::updateDirty(registry_); }

        /**
         * Makes @p entity the sole enabled directional light in this scene.
         * Point and spot components remain untouched until those light paths
         * are implemented by the renderer.
         */
        void setActiveDirectionalLight(const Entity entity) {
            if (!registry_.valid(entity) || !registry_.has<LightComponent>(entity) ||
                registry_.get<LightComponent>(entity).type != LightType::Directional) {
                throw std::invalid_argument("Active light must be a directional LightComponent");
            }
            registry_.view<LightComponent>([&](const Entity candidate, LightComponent& light) {
                if (light.type != LightType::Directional) return;
                const bool enabled = candidate == entity;
                if (light.enabled != enabled) {
                    light.enabled = enabled;
                    registry_.markChanged<LightComponent>(candidate);
                }
            });
        }

        /** High-level scene persistence helpers. */
        void save(const std::filesystem::path &path) const;

        void load(const std::filesystem::path &path);

        /** High-level editor facade; it does not expose the underlying Registry. */
        [[nodiscard]] SceneEditor editor() noexcept;

        [[nodiscard]] SceneEditor editor() const noexcept;

        /** Finds a named object, or returns nullptr when it does not exist. */
        [[nodiscard]] GameObject *find(const std::string &name) noexcept {
            const auto it = names_.find(name); //NOLINT
            if (it == names_.end()) {
                return nullptr;
            }
            return find(it->second);
        }

        /** Finds an object by its stable object identifier. */
        [[nodiscard]] GameObject *find(const ObjectId objectId) const noexcept {
            for (const auto &object: objects_) {
                if (object->objectId() == objectId) {
                    return object.get();
                }
            }
            return nullptr;
        }

        [[nodiscard]] Entity findEntity(const ObjectId objectId) const noexcept {
            for (const auto &object: objects_) {
                if (object->objectId() == objectId) {
                    return object->entity();
                }
            }
            return NullEntity;
        }

        /** Finds the high-level object represented by an ECS entity. */
        [[nodiscard]] GameObject *findByEntity(const Entity entity) noexcept {
            for (const auto &object: objects_) {
                if (object->entity() == entity) {
                    return object.get();
                }
            }
            return nullptr;
        }

        [[nodiscard]] const GameObject *findByEntity(const Entity entity) const noexcept {
            for (const auto &object: objects_) {
                if (object->entity() == entity) {
                    return object.get();
                }
            }
            return nullptr;
        }

        /** Returns a high-level object for editor/runtime code. */
        [[nodiscard]] GameObject &edit(const Entity entity) {
            auto *object = findByEntity(entity);
            if (object == nullptr) {
                throw std::out_of_range("Scene entity is not a GameObject");
            }
            return *object;
        }

        [[nodiscard]] const GameObject &edit(const Entity entity) const {
            const auto *object = findByEntity(entity);
            if (object == nullptr) {
                throw std::out_of_range("Scene entity is not a GameObject");
            }
            return *object;
        }

        [[nodiscard]] bool valid(const Entity entity) const noexcept {
            return findByEntity(entity) != nullptr;
        }

        /**
         * Returns whether the scene has a camera that can drive the game view.
         *
         * A primary camera must also have a Transform and supported,
         * perspective settings.  The renderer can fall back safely when this
         * is false, while editor UI can expose the scene-authoring problem.
         */
        [[nodiscard]] bool hasUsablePrimaryCamera() const {
            bool found = false;
            registry_.view<CameraComponent, Transform>(
                [&](const Entity, const CameraComponent& camera, const Transform&) {
                    found = found || (camera.primary && camera.isPerspective() && camera.isValid());
                });
            return found;
        }

        void destroy(const Entity entity) {
            const auto it = std::ranges::find_if(objects_, //NOLINT
                                                 [entity](const auto &object) { return object->entity() == entity; });
            if (it == objects_.end()) {
                return;
            }

            // A parent owns the lifetime of its descendants. Work on a copy as
            // destroy() erases entries from objects_.
            const ObjectId objectId = (*it)->objectId();
            for (const Actor &child : childrenOf(Actor{*this, objectId})) {
                destroy(child);
            }

            // Recursive destruction can reallocate objects_, invalidating it.
            const auto owner = std::ranges::find_if(objects_, [objectId](const auto &object) {
                return object->objectId() == objectId;
            });
            if (owner == objects_.end()) return;

            names_.erase((*owner)->name()); //NOLINT
            // Destroy the ECS entity before removing its owning wrapper. Merely
            // detaching it would make it disappear from the hierarchy while the
            // renderer could still find and draw the live registry entity.
            (*owner)->destroy(); //NOLINT
            objects_.erase(owner);
        }

        [[nodiscard]] std::uint64_t structuralRevision() const noexcept {
            return registry_.structuralRevision();
        }

        /** Monotonic revision advanced by every explicit ECS mutation. */
        [[nodiscard]] std::uint64_t mutationRevision() const noexcept {
            return registry_.mutationRevision();
        }

        template<typename Func>
        void eachObject(Func &&func) {
            for (const auto &object: objects_) {
                func(*object);
            }
        }

        template<typename Func>
        void eachObject(Func &&func) const {
            for (const auto &object: objects_) {
                func(*object);
            }
        }

        template<typename Func>
        void eachActor(Func &&func) {
            for (const auto &object: objects_) {
                func(Actor{*this, object->objectId()});
            }
        }

        /** Number of objects created through the high-level Scene API. */
        [[nodiscard]] std::size_t objectCount() const noexcept { return objects_.size(); }

        [[nodiscard]] UI::Canvas &uiCanvas() noexcept { return canvas_; }
        [[nodiscard]] const UI::Canvas &uiCanvas() const noexcept { return canvas_; }
        [[nodiscard]] const UI::UIFontAtlas &uiFontAtlas() const noexcept { return fontAtlas_; }
        [[nodiscard]] UI::UIFontAtlas &uiFontAtlas() noexcept { return fontAtlas_; }
        [[nodiscard]] UI::Interface ui() noexcept { return UI::Interface{canvas_, fontAtlas_}; }

        // Preset metadata can outlive a registry replacement performed by the
        // scene serializer. Resolve particle state from the current registry so
        // loading a non-particle scene cannot address an old entity id.
        [[nodiscard]] Entity particleEntity() const noexcept {
            if (particleEntity_ != NullEntity &&
                (registry_.has<ParticleEmitterComponent>(particleEntity_) ||
                 registry_.has<SmokeEmitterComponent>(particleEntity_))) {
                return particleEntity_;
            }
            Entity found = NullEntity;
            registry_.view<SmokeEmitterComponent>([&](const Entity entity,
                                                      const SmokeEmitterComponent &) {
                if (found == NullEntity) {
                    found = entity;
                }
            });
            registry_.view<ParticleEmitterComponent>([&](const Entity entity,
                                                         const ParticleEmitterComponent &) {
                if (found == NullEntity) {
                    found = entity;
                }
            });
            return found;
        }

        [[nodiscard]] bool isParticleScene() const noexcept {
            return particleEntity() != NullEntity;
        }

        [[nodiscard]] const Particles::ParticleEmitter &particleEmitter() const noexcept {
            return particleEmitter_;
        }

    protected:
        /** Engine-only mesh creation primitive for scene presets. */
        [[nodiscard]] GameObject &createMeshObject(std::string name,
                                                    std::shared_ptr<const Mesh> mesh,
                                                    PBRMaterial material = {});

        // Scene subclasses are engine-owned content layers. They may assemble
        // entities directly; application code uses Actor instead.
        [[nodiscard]] Registry &registry() noexcept { return registry_; }
        [[nodiscard]] const Registry &registry() const noexcept { return registry_; }
        void setParticleEntity(const Entity entity) noexcept { particleEntity_ = entity; }

        void setParticleEmitter(const Particles::ParticleEmitter &emitter) noexcept {
            particleEmitter_ = emitter;
            particleScene_ = true;
        }

        void setSmokeEmitter(const Particles::SmokeEmitter &emitter) noexcept {
            particleEmitter_ = static_cast<Particles::ParticleEmitter>(emitter);
            particleScene_ = true;
        }

    private:
        friend class Application;
        friend class SceneSerializer;
        friend class Renderer;
        friend class ScriptSystem;
        friend class PhysicsSystem;

        void rebuildObjectHandles();

        void setContent(Assets::Content &content) noexcept { content_ = &content; }

        void detachObjectHandles() noexcept {
            for (const auto &object: objects_) {
                object->detach();
            }
            objects_.clear();
            names_.clear();
        }

        Registry registry_;
        std::vector<std::unique_ptr<GameObject> > objects_;
        std::unordered_map<std::string, ObjectId> names_;
        UI::Canvas canvas_{800, 600}; //NOLINT
        UI::UIFontAtlas fontAtlas_;
        Particles::ParticleEmitter particleEmitter_{};
        Entity particleEntity_{NullEntity};
        Assets::Content *content_{};
        bool particleScene_ = false;
    };
} // namespace Engine
