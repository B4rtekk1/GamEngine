#pragma once

#include "Entity.h"

#include <unordered_map>
#include <memory>
#include <cassert>
#include <utility>

namespace Engine {

class IComponentPool {
public:
    virtual ~IComponentPool() = default;

    virtual void remove(Entity entity) = 0;

    [[nodiscard]] virtual bool has(Entity entity) const = 0;
};

template<typename T>
class ComponentPool final : public IComponentPool {
public:
    template<typename... Args>
    T &add(Entity entity, Args &&... args) {
        auto [it, inserted] = m_components.emplace(
            entity,
            T{std::forward<Args>(args)...});

        assert(inserted && "Component already exists for this entity");
        return it->second;
    }

    void remove(Entity entity) override {
        m_components.erase(entity);
    }

    [[nodiscard]] bool has(Entity entity) const override {
        return m_components.contains(entity);
    }

    T& get(Entity entity) {
        auto it = m_components.find(entity);

        assert(it != m_components.end() && "Component does not exist for this entity");
        return it->second;
    }

    const T& get(Entity entity) const {
        const auto it = m_components.find(entity);

        assert(it != m_components.end() && "Component does not exist for this entity");
        return it->second;
    }

    auto begin() {
        return m_components.begin();
    }

    auto end() {
        return m_components.end();
    }

    auto begin() const {
        return m_components.begin();
    }

    auto end() const {
        return m_components.end();
    }

private:
    std::unordered_map<Entity, T> m_components;
};

} // namespace Engine
