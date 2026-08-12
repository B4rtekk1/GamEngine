#pragma once

#include "Entity.h"

#include <memory>
#include <cassert>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

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
        assert(!has(entity) && "Component already exists for this entity");
        if (has(entity)) {
            return get(entity);
        }

        const auto index = m_components.size();
        ensureSparseCapacity(entity);
        m_sparse[static_cast<std::size_t>(entity)] = index;
        m_entities.push_back(entity);
        m_components.emplace_back(std::forward<Args>(args)...);
        return m_components.back();
    }

    void remove(Entity entity) override {
        if (!has(entity)) {
            return;
        }

        const auto index = m_sparse[static_cast<std::size_t>(entity)];
        const auto lastIndex = m_components.size() - 1;

        if (index != lastIndex) {
            m_components[index] = std::move(m_components[lastIndex]);

            const Entity movedEntity = m_entities[lastIndex];
            m_entities[index] = movedEntity;
            m_sparse[static_cast<std::size_t>(movedEntity)] = index;
        }

        m_components.pop_back();
        m_entities.pop_back();
        m_sparse[static_cast<std::size_t>(entity)] = InvalidIndex;
    }

    [[nodiscard]] bool has(Entity entity) const override {
        const auto entityIndex = static_cast<std::size_t>(entity);
        return entityIndex < m_sparse.size()
            && m_sparse[entityIndex] != InvalidIndex
            && m_sparse[entityIndex] < m_entities.size()
            && m_entities[m_sparse[entityIndex]] == entity;
    }

    T& get(Entity entity) {
        assert(has(entity) && "Component does not exist for this entity");
        return m_components[m_sparse[static_cast<std::size_t>(entity)]];
    }

    const T& get(Entity entity) const {
        assert(has(entity) && "Component does not exist for this entity");
        return m_components[m_sparse[static_cast<std::size_t>(entity)]];
    }

    [[nodiscard]] const std::vector<Entity>& entities() const {
        return m_entities;
    }

private:
    static constexpr std::size_t InvalidIndex = std::numeric_limits<std::size_t>::max();

    void ensureSparseCapacity(Entity entity) {
        const auto entityIndex = static_cast<std::size_t>(entity);
        assert(static_cast<Entity>(entityIndex) == entity && "Entity value exceeds sparse-set index range");

        if (entityIndex >= m_sparse.size()) {
            m_sparse.resize(entityIndex + 1, InvalidIndex);
        }
    }

    std::vector<T> m_components;
    std::vector<Entity> m_entities;
    std::vector<std::size_t> m_sparse;
};

} // namespace Engine
