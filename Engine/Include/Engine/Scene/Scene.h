#pragma once

#include "Engine/ECS/GameObject.h"
#include "Engine/ECS/Registry.h"
#include "Engine/Renderer/Particles/ParticleSystem.h"
#include "Engine/ECS/Components/ParticleEmitterComponent.h"
#include "Engine/ECS/Components/ColorPickerComponent.h"
#include "Engine/UI/Canvas.h"
#include "Engine/UI/Vulkan/UIFontAtlas.h"
#include "Engine/Scene/Components/IdentityComponents.h"
#include "Engine/Renderer/Geometry/Mesh.h"
#include "Engine/Renderer/Materials/PBRMaterial.h"

#include <memory>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace Engine { class SceneSerializer; }

namespace Engine {

// Runtime scene data. Content creation belongs to ScenePresets (or to the
// application), rather than to this data container.
class Scene {
public:
    /** Creates a named game object with the standard transform and renderer components. */
    [[nodiscard]] GameObject& create(std::string name) {
        if (name.empty()) {
            throw std::invalid_argument("Scene object name cannot be empty");
        }
        if (find(name) != nullptr) {
            throw std::invalid_argument("Scene object already exists: " + name);
        }

        auto object = std::make_unique<GameObject>(registry, name);
        object->spawn();
        GameObject& result = *object;
        registry.add<NameComponent>(result.entity(), NameComponent{.value = name});
        registry.add<UUIDComponent>(result.entity(), UUIDComponent{.value = createUUID()});
        names_[result.name()] = result.objectId();
        objects_.push_back(std::move(object));
        return result;
    }

    /** Preferred spelling for application code. */
    [[nodiscard]] GameObject& createGameObject(std::string name) {
        return create(std::move(name));
    }

    /** Creates a renderable object without exposing Registry or component wiring. */
    [[nodiscard]] GameObject& createMeshObject(std::string name,
                                                std::shared_ptr<const Mesh> mesh,
                                                PBRMaterial material = {});

    /** High-level scene persistence helpers. */
    void save(const std::filesystem::path& path) const;
    void load(const std::filesystem::path& path);

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

    /** Number of objects created through the high-level Scene API. */
    [[nodiscard]] std::size_t objectCount() const noexcept { return objects_.size(); }

    Registry registry;

    [[nodiscard]] UI::Canvas& uiCanvas() noexcept { return canvas_; }
    [[nodiscard]] const UI::Canvas& uiCanvas() const noexcept { return canvas_; }
    [[nodiscard]] const UI::UIFontAtlas& uiFontAtlas() const noexcept { return fontAtlas_; }
    [[nodiscard]] UI::UIFontAtlas& uiFontAtlas() noexcept { return fontAtlas_; }

    // Preset metadata can outlive a registry replacement performed by the
    // scene serializer. Resolve particle state from the current registry so
    // loading a non-particle scene cannot address an old entity id.
    [[nodiscard]] Entity particleEntity() const noexcept {
        if (particleEntity_ != NullEntity &&
            registry.has<ParticleEmitterComponent>(particleEntity_)) {
            return particleEntity_;
        }
        Entity found = NullEntity;
        registry.view<ParticleEmitterComponent>([&](const Entity entity,
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
    void setParticleEntity(const Entity entity) noexcept { particleEntity_ = entity; }

    void setParticleEmitter(Particles::ParticleEmitter emitter) noexcept {
        particleEmitter_ = emitter;
        particleScene_ = true;
    }

private:
    std::vector<std::unique_ptr<GameObject>> objects_;
    std::unordered_map<std::string, ObjectId> names_;
    UI::Canvas canvas_{800, 600};
    UI::UIFontAtlas fontAtlas_{};
    Particles::ParticleEmitter particleEmitter_{};
    Entity particleEntity_{NullEntity};
    bool particleScene_ = false;
};

} // namespace Engine
