#pragma once

#include "Engine/Scene/Scene.h"

#include <utility>

namespace Engine {

/** Narrow editor-facing scene access. Keeps Registry out of editor code. */
class SceneEditor final {
public:
    explicit SceneEditor(Scene& scene) noexcept : scene_(&scene) {}
    explicit SceneEditor(const Scene& scene) noexcept
        : scene_(const_cast<Scene*>(&scene)) {}

    [[nodiscard]] std::size_t size() const noexcept { return scene_->objectCount(); }
    [[nodiscard]] bool valid(Entity entity) const { return scene_->valid(entity); }
    void destroy(Entity entity) { scene_->destroy(entity); }
    [[nodiscard]] Entity duplicate(Entity entity) { return scene_->duplicate(entity).entity(); }
    [[nodiscard]] std::uint64_t structuralRevision() const noexcept {
        return scene_->structuralRevision();
    }
    [[nodiscard]] std::uint64_t mutationRevision() const noexcept {
        return scene_->mutationRevision();
    }

    template<typename T> [[nodiscard]] bool has(Entity entity) const { return scene_->edit(entity).has<T>(); }
    template<typename T> T& get(Entity entity) { return scene_->edit(entity).get<T>(); }
    template<typename T> const T& get(Entity entity) const { return scene_->edit(entity).get<T>(); }
    template<typename T, typename... Args> T& add(Entity entity, Args&&... args) {
        if constexpr (std::is_same_v<T, LightComponent>) {
            T& light = scene_->edit(entity).addLight(std::forward<Args>(args)...);
            if (light.type == LightType::Directional && light.enabled) {
                scene_->setActiveDirectionalLight(entity);
            }
            return light;
        } else if constexpr (std::is_same_v<T, MeshRendererComponent>) {
            if (scene_->edit(entity).has<LightComponent>()) {
                throw std::logic_error("A LightComponent cannot have a MeshRenderer");
            }
        }
        return scene_->edit(entity).add<T>(std::forward<Args>(args)...);
    }
    template<typename T> void remove(Entity entity) { scene_->edit(entity).remove<T>(); }
    template<typename T, typename Func> void modify(Entity entity, Func&& func) {
        scene_->edit(entity).modify<T>(std::forward<Func>(func));
        if constexpr (std::is_same_v<T, LightComponent>) {
            const LightComponent& light = scene_->edit(entity).get<LightComponent>();
            if (light.type == LightType::Directional && light.enabled) {
                scene_->setActiveDirectionalLight(entity);
            }
        }
    }
    void setActiveDirectionalLight(Entity entity) { scene_->setActiveDirectionalLight(entity); }
    template<typename... Components, typename Func> void view(Func&& func) {
        scene_->eachObject([&](const GameObject& object) {
            if constexpr (sizeof...(Components) == 0) {
                func(object.entity());
            } else if ((object.template has<Components>() && ...)) {
                func(object.entity(), object.template get<Components>()...);
            }
        });
    }

private:
    Scene* scene_;
};

} // namespace Engine

namespace Engine {
inline SceneEditor Scene::editor() noexcept { return SceneEditor{*this}; }
inline SceneEditor Scene::editor() const noexcept { return SceneEditor{*this}; }
} // namespace Engine
