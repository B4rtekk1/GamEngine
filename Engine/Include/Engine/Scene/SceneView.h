#pragma once

#include "Engine/Scene/Scene.h"

namespace Engine {

/** Read-only scene access for inspection, without an escape hatch to mutation. */
class SceneView final {
public:
    explicit SceneView(const Scene& scene) noexcept : scene_(&scene) {}

    [[nodiscard]] std::size_t size() const noexcept { return scene_->objectCount(); }
    [[nodiscard]] bool valid(const Entity entity) const noexcept { return scene_->valid(entity); }
    [[nodiscard]] std::uint64_t structuralRevision() const noexcept {
        return scene_->structuralRevision();
    }
    [[nodiscard]] std::uint64_t mutationRevision() const noexcept {
        return scene_->mutationRevision();
    }

    template<typename T> [[nodiscard]] bool has(const Entity entity) const {
        return scene_->edit(entity).template has<T>();
    }

    template<typename T> [[nodiscard]] const T& read(const Entity entity) const {
        return scene_->edit(entity).template get<T>();
    }

    template<typename... Components, typename Func> void view(Func&& func) const {
        scene_->eachObject([&](const GameObject& object) {
            if constexpr (sizeof...(Components) == 0) {
                func(object.entity());
            } else if ((object.template has<Components>() && ...)) {
                func(object.entity(), object.template get<Components>()...);
            }
        });
    }

private:
    const Scene* scene_;
};

inline SceneView Scene::view() const noexcept { return SceneView{*this}; }

} // namespace Engine
