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
        return scene_->edit(entity).add<T>(std::forward<Args>(args)...);
    }
    template<typename T> void remove(Entity entity) { scene_->edit(entity).remove<T>(); }
    template<typename T, typename Func> void modify(Entity entity, Func&& func) {
        scene_->edit(entity).modify<T>(std::forward<Func>(func));
    }
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
