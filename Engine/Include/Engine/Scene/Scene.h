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
#include "Engine/Scene/Prefab.h"
#include "Engine/UI/Interface.h"
#include "Engine/ECS/Components/CameraComponent.h"
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

namespace Engine { class SceneSerializer; }
namespace Engine::Assets { class Content; }

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
    /** Creates a named game object with the standard transform and renderer components. */
    [[nodiscard]] GameObject& create(std::string name) {
        if (name.empty()) {
            throw std::invalid_argument("Scene object name cannot be empty");
        }
        const std::string baseName = name;
        std::size_t suffix = 2;
        while (find(name) != nullptr) name = baseName + " " + std::to_string(suffix++);

        auto object = std::unique_ptr<GameObject>(new GameObject(registry_, name));
        object->spawn();
        GameObject& result = *object;
        registry_.add<NameComponent>(result.entity(), NameComponent{.value = name});
        registry_.add<UUIDComponent>(result.entity(), UUIDComponent{.value = createUUID()});
        names_[result.name()] = result.objectId();
        objects_.push_back(std::move(object));
        return result;
    }

    /** Preferred spelling for application code. */
    [[nodiscard]] GameObject& createGameObject(std::string name) {
        return create(std::move(name));
    }

    /** High-level actor creation without exposing the ECS entity handle. */
    [[nodiscard]] Actor createActor(std::string name) {
        auto& object = create(std::move(name));
        return Actor{*this, object.objectId()};
    }

    /** Duplicates an actor without exposing its ECS entity identifier. */
    [[nodiscard]] Actor duplicate(const Actor& actor) {
        if (actor.scene_ != this || !actor.valid()) return {};
        auto& object = duplicate(actor.object().entity());
        return Actor{*this, object.objectId()};
    }

    /** Creates a copy of an existing object with a fresh name and UUID. */
    [[nodiscard]] GameObject& duplicate(const Entity entity) {
        GameObject* source = findByEntity(entity);
        if (source == nullptr) throw std::out_of_range("Scene entity is not a GameObject");

        std::string name = source->name();
        if (registry_.has<NameComponent>(entity)) name = registry_.get<NameComponent>(entity).value;
        const std::string baseName = name;
        std::size_t suffix = 2;
        while (find(name) != nullptr) name = baseName + " " + std::to_string(suffix++);

        auto object = std::make_unique<GameObject>(source->clone());
        object->setName(name);
        const Entity copiedEntity = object->entity();
        registry_.modify<NameComponent>(copiedEntity, [&](auto& value) { value.value = name; });
        registry_.modify<UUIDComponent>(copiedEntity, [&](auto& value) { value.value = createUUID(); });
        names_[name] = object->objectId();
        objects_.push_back(std::move(object));
        return *objects_.back();
    }

    /** Creates a renderable object without exposing Registry or component wiring. */
    [[nodiscard]] GameObject& createMeshObject(std::string name,
                                                std::shared_ptr<const Mesh> mesh,
                                                PBRMaterial material = {});

    [[nodiscard]] GameObject& createMesh(std::string name,
                                          std::shared_ptr<const Mesh> mesh,
                                          PBRMaterial material = {}) {
        return createMeshObject(std::move(name), std::move(mesh), std::move(material));
    }

    /** Loads a model through Content and creates an actor in one operation. */
    [[nodiscard]] Actor createModel(std::string name,
                                    std::filesystem::path path,
                                    Assets::Content& content);

    /** Loads a model using the Content service attached to this scene. */
    [[nodiscard]] Actor createModel(std::string name, std::filesystem::path path);

    /** Loads a model prefab and instantiates it in one operation. */
    [[nodiscard]] Actor createPrefab(std::string name,
                                     std::filesystem::path path,
                                     PBRMaterial material = {});

    [[nodiscard]] Actor createCube(std::string name, PBRMaterial material = {});
    [[nodiscard]] Actor createPrefab(std::string name, const Prefab& prefab);

    [[nodiscard]] Actor findActor(const std::string& name) noexcept {
        auto* object = find(name);
        return object == nullptr ? Actor{} : Actor{*this, object->objectId()};
    }

    [[nodiscard]] Actor createCameraActor(std::string name, CameraComponent camera = {}) {
        auto& object = create(std::move(name));
        object.addCamera(std::move(camera));
        return Actor{*this, object.objectId()};
    }

    [[nodiscard]] Actor createLightActor(std::string name, LightComponent light = {}) {
        auto& object = create(std::move(name));
        object.addLight(std::move(light));
        return Actor{*this, object.objectId()};
    }

    /** Renames an actor while keeping name lookup consistent. */
    void rename(const Actor& actor, std::string name) {
        if (actor.scene_ != this || !actor.valid()) return;
        if (name.empty()) throw std::invalid_argument("Scene object name cannot be empty");
        if (auto* existing = find(name); existing != nullptr && existing->objectId() != actor.objectId_) {
            throw std::invalid_argument("Scene object name is already in use: " + name);
        }
        auto* object = find(actor.objectId_);
        names_.erase(object->name());
        object->setName(std::move(name));
        names_[object->name()] = object->objectId();
    }

    /** Destroys an actor through the high-level runtime API. */
    void destroy(const Actor& actor) noexcept {
        if (actor.scene_ != this || actor.objectId_ == NullObjectId) return;
        if (auto* object = find(actor.objectId_)) destroy(object->entity());
    }

    [[nodiscard]] GameObject& createCamera(std::string name, CameraComponent camera = {}) {
        auto& object = create(std::move(name));
        object.addCamera(std::move(camera));
        return object;
    }

    [[nodiscard]] GameObject& createLight(std::string name, LightComponent light = {}) {
        auto& object = create(std::move(name));
        object.addLight(std::move(light));
        return object;
    }

    /** High-level scene persistence helpers. */
    void save(const std::filesystem::path& path) const;
    void load(const std::filesystem::path& path);

    /** High-level editor facade; it does not expose the underlying Registry. */
    [[nodiscard]] SceneEditor editor() noexcept;
    [[nodiscard]] SceneEditor editor() const noexcept;

    /** Finds a named object, or returns nullptr when it does not exist. */
    [[nodiscard]] GameObject* find(const std::string& name) noexcept {
        const auto it = names_.find(name);
        if (it == names_.end()) return nullptr;
        return find(it->second);
    }

    /** Finds an object by its stable object identifier. */
    [[nodiscard]] GameObject* find(const ObjectId objectId) noexcept {
        for (const auto& object : objects_) {
            if (object->objectId() == objectId) return object.get();
        }
        return nullptr;
    }

    /** Finds the high-level object represented by an ECS entity. */
    [[nodiscard]] GameObject* findByEntity(const Entity entity) noexcept {
        for (const auto& object : objects_) {
            if (object->entity() == entity) return object.get();
        }
        return nullptr;
    }

    [[nodiscard]] const GameObject* findByEntity(const Entity entity) const noexcept {
        for (const auto& object : objects_) {
            if (object->entity() == entity) return object.get();
        }
        return nullptr;
    }

    /** Returns a high-level object for editor/runtime code. */
    [[nodiscard]] GameObject& edit(const Entity entity) {
        auto* object = findByEntity(entity);
        if (object == nullptr) throw std::out_of_range("Scene entity is not a GameObject");
        return *object;
    }

    [[nodiscard]] const GameObject& edit(const Entity entity) const {
        const auto* object = findByEntity(entity);
        if (object == nullptr) throw std::out_of_range("Scene entity is not a GameObject");
        return *object;
    }

    [[nodiscard]] bool valid(const Entity entity) const noexcept {
        return findByEntity(entity) != nullptr;
    }

    void destroy(const Entity entity) noexcept {
        const auto it = std::find_if(objects_.begin(), objects_.end(),
            [entity](const auto& object) { return object->entity() == entity; });
        if (it == objects_.end()) return;

        // The renderer requires one primary camera every frame. Deleting the
        // only one would leave the scene in an invalid state and make the
        // next frame fail while building the camera uniforms.
        if (registry_.has<CameraComponent>(entity) &&
            registry_.get<CameraComponent>(entity).primary) {
            std::size_t primaryCameraCount = 0;
            registry_.view<CameraComponent>([&](const Entity, const CameraComponent& camera) {
                if (camera.primary) ++primaryCameraCount;
            });
            if (primaryCameraCount <= 1) return;
        }

        names_.erase((*it)->name());
        // Destroy the ECS entity before removing its owning wrapper. Merely
        // detaching it would make it disappear from the hierarchy while the
        // renderer could still find and draw the live registry entity.
        (*it)->destroy();
        objects_.erase(it);
    }

    [[nodiscard]] std::uint64_t structuralRevision() const noexcept {
        return registry_.structuralRevision();
    }

    template<typename Func>
    void eachObject(Func&& func) {
        for (const auto& object : objects_) func(*object);
    }

    template<typename Func>
    void eachObject(Func&& func) const {
        for (const auto& object : objects_) func(*object);
    }

    template<typename Func>
    void eachActor(Func&& func) {
        for (const auto& object : objects_) func(Actor{*this, object->objectId()});
    }

    /** Number of objects created through the high-level Scene API. */
    [[nodiscard]] std::size_t objectCount() const noexcept { return objects_.size(); }

    [[nodiscard]] UI::Canvas& uiCanvas() noexcept { return canvas_; }
    [[nodiscard]] const UI::Canvas& uiCanvas() const noexcept { return canvas_; }
    [[nodiscard]] const UI::UIFontAtlas& uiFontAtlas() const noexcept { return fontAtlas_; }
    [[nodiscard]] UI::UIFontAtlas& uiFontAtlas() noexcept { return fontAtlas_; }
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
                                                  const SmokeEmitterComponent&) {
            if (found == NullEntity) found = entity;
        });
        registry_.view<ParticleEmitterComponent>([&](const Entity entity,
                                                     const ParticleEmitterComponent&) {
            if (found == NullEntity) found = entity;
        });
        return found;
    }
    [[nodiscard]] bool isParticleScene() const noexcept {
        return particleEntity() != NullEntity;
    }
    [[nodiscard]] const Particles::ParticleEmitter& particleEmitter() const noexcept {
        return particleEmitter_;
    }

protected:
    // Scene subclasses are engine-owned content layers. They may assemble
    // entities, while application code uses GameObject instead.
    [[nodiscard]] Registry& registry() noexcept { return registry_; }
    [[nodiscard]] const Registry& registry() const noexcept { return registry_; }
    void setParticleEntity(const Entity entity) noexcept { particleEntity_ = entity; }

    void setParticleEmitter(Particles::ParticleEmitter emitter) noexcept {
        particleEmitter_ = emitter;
        particleScene_ = true;
    }

    void setSmokeEmitter(Particles::SmokeEmitter emitter) noexcept {
        particleEmitter_ = emitter;
        particleScene_ = true;
    }

private:
    friend class Application;
    friend class SceneSerializer;
    friend class Renderer;
    friend class ScriptSystem;
    friend class PhysicsSystem;

    void rebuildObjectHandles();
    void setContent(Assets::Content& content) noexcept { content_ = &content; }
    void detachObjectHandles() noexcept {
        for (const auto& object : objects_) object->detach();
        objects_.clear();
        names_.clear();
    }

    Registry registry_;
    std::vector<std::unique_ptr<GameObject>> objects_;
    std::unordered_map<std::string, ObjectId> names_;
    UI::Canvas canvas_{800, 600};
    UI::UIFontAtlas fontAtlas_{};
    Particles::ParticleEmitter particleEmitter_{};
    Entity particleEntity_{NullEntity};
    Assets::Content* content_{};
    bool particleScene_ = false;
};

} // namespace Engine

#include "Engine/Scene/SceneEditor.h"

namespace Engine {
inline SceneEditor Scene::editor() noexcept { return SceneEditor{*this}; }
inline SceneEditor Scene::editor() const noexcept { return SceneEditor{*this}; }
}
