#pragma once

#include "Engine/ECS/Registry.h"

#include <utility>

namespace Engine {

/** Narrow editor-facing scene access. Keeps Registry out of editor code. */
class SceneEditor final {
public:
    explicit SceneEditor(Registry& registry) noexcept : registry_(&registry) {}
    explicit SceneEditor(const Registry& registry) noexcept
        : registry_(const_cast<Registry*>(&registry)) {}

    [[nodiscard]] std::size_t size() const noexcept { return registry_->size(); }
    [[nodiscard]] bool valid(Entity entity) const { return registry_->valid(entity); }
    [[nodiscard]] std::uint64_t structuralRevision() const noexcept {
        return registry_->structuralRevision();
    }

    template<typename T> [[nodiscard]] bool has(Entity entity) const { return registry_->has<T>(entity); }
    template<typename T> T& get(Entity entity) { return registry_->get<T>(entity); }
    template<typename T> const T& get(Entity entity) const { return registry_->get<T>(entity); }
    template<typename T, typename... Args> T& add(Entity entity, Args&&... args) {
        return registry_->add<T>(entity, std::forward<Args>(args)...);
    }
    template<typename T, typename Func> void modify(Entity entity, Func&& func) {
        registry_->modify<T>(entity, std::forward<Func>(func));
    }
    template<typename... Components, typename Func> void view(Func&& func) {
        registry_->view<Components...>(std::forward<Func>(func));
    }

private:
    Registry* registry_;
};

} // namespace Engine
