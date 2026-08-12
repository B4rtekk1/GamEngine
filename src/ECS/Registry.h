#pragma once

#include "Entity.h"
#include "Componentpool.h"

#include <unordered_map>
#include <unordered_set>
#include <typeindex>
#include <memory>
#include <cassert>
#include <functional>
#include <ranges>
#include <tuple>
#include <utility>


class Registry {
public:
    Entity create() {
        const Entity entity = m_nextEntity++;
        m_entities.insert(entity);
        return entity;
    }

    void destroy(Entity entity) {
        if (!m_entities.erase(entity)) {
            return;
        }

        for (auto &pool: m_componentPools | std::views::values) {
            pool->remove(entity);
        }
    }

    [[nodiscard]]
    bool valid(Entity entity) const {
        return entity != NullEntity && m_entities.contains(entity);
    }

    template<typename T, typename... Args>
    T &add(Entity entity, Args &&... args) {
        assert(valid(entity) && "Cannot add a component to an invalid entity");
        return getOrCreatePool<T>().add(entity, std::forward<Args>(args)...);
    }

    template<typename T>
    void remove(Entity entity) {
        assert(valid(entity) && "Cannot remove a component from an invalid entity");

        if (auto *pool = findPool<T>()) {
            pool->remove(entity);
        }
    }

    template<typename T>
    [[nodiscard]]
    bool has(Entity entity) const {
        if (!valid(entity)) {
            return false;
        }

        const auto *pool = findPool<T>();
        return pool != nullptr && pool->has(entity);
    }

    template<typename T>
    T &get(Entity entity) {
        assert(valid(entity) && "Cannot get a component from an invalid entity");

        auto *pool = findPool<T>();
        assert(pool != nullptr && "Component type is not registered");
        return pool->get(entity);
    }

    template<typename T>
    const T &get(Entity entity) const {
        assert(valid(entity) && "Cannot get a component from an invalid entity");

        const auto *pool = findPool<T>();
        assert(pool != nullptr && "Component type is not registered");
        return pool->get(entity);
    }

    template<typename... Components, typename Func>
    void view(Func &&func) {
        if constexpr (sizeof...(Components) == 0) {
            for (const Entity entity : m_entities) {
                std::invoke(func, entity);
            }
        } else {
            using FirstComponent = std::tuple_element_t<0, std::tuple<Components...>>;
            auto *firstPool = findPool<FirstComponent>();

            if (firstPool == nullptr) {
                return;
            }

            for (auto &[entity, firstComponent] : *firstPool) {
                if ((has<Components>(entity) && ...)) {
                    std::invoke(func, entity, get<Components>(entity)...);
                }
            }
        }
    }

private:
    template<typename T>
    ComponentPool<T> &getOrCreatePool() {
        const std::type_index type = typeid(T);
        const auto it = m_componentPools.try_emplace(
            type,
            std::make_unique<ComponentPool<T>>()).first;

        return static_cast<ComponentPool<T> &>(*it->second);
    }

    template<typename T>
    ComponentPool<T> *findPool() {
        const auto it = m_componentPools.find(typeid(T));
        return it == m_componentPools.end()
            ? nullptr
            : static_cast<ComponentPool<T> *>(it->second.get());
    }

    template<typename T>
    const ComponentPool<T> *findPool() const {
        const auto it = m_componentPools.find(typeid(T));
        return it == m_componentPools.end()
            ? nullptr
            : static_cast<const ComponentPool<T> *>(it->second.get());
    }

private:
    Entity m_nextEntity = 1;

    std::unordered_set<Entity> m_entities;

    std::unordered_map<
        std::type_index,
        std::unique_ptr<IComponentPool>
    > m_componentPools;
};
