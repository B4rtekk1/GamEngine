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

namespace Engine {

/**
 * @brief Owns entities and their component pools.
 *
 * Registry provides entity lifetime management and type-safe component
 * operations. Each component type is stored in its own ComponentPool.
 *
 * @note Entity identifiers are monotonically increasing and are not currently
 * recycled.
 */
class Registry {
public:
    /**
     * @brief Creates a new entity.
     *
     * @return Identifier of the newly created entity.
     */
    Entity create() {
        const Entity entity = m_nextEntity++;
        m_entities.insert(entity);
        return entity;
    }

    /**
     * @brief Destroys an entity and removes all of its components.
     *
     * Calling this function with an invalid or already destroyed entity has no
     * effect.
     *
     * @param entity Entity to destroy.
     */
    void destroy(Entity entity) {
        if (!m_entities.erase(entity)) {
            return;
        }

        for (auto &pool: m_componentPools | std::views::values) {
            pool->remove(entity);
        }
    }

    /**
     * @brief Checks whether an entity is currently alive.
     *
     * @param entity Entity identifier to validate.
     * @return true if the entity exists and is not @ref NullEntity.
     */
    [[nodiscard]]
    bool valid(Entity entity) const {
        return entity != NullEntity && m_entities.contains(entity);
    }

    /**
     * @brief Adds and constructs a component for an entity.
     *
     * The appropriate component pool is created lazily when the component type
     * is used for the first time.
     *
     * @tparam T Component type.
     * @tparam Args Component constructor argument types.
     * @param entity Entity receiving the component.
     * @param args Arguments forwarded to the component constructor.
     * @return Reference to the newly created component.
     *
     * @pre @p entity is valid.
     * @pre The entity does not already own a component of type @p T.
     */
    template<typename T, typename... Args>
    T &add(Entity entity, Args &&... args) {
        assert(valid(entity) && "Cannot add a component to an invalid entity");
        return getOrCreatePool<T>().add(entity, std::forward<Args>(args)...);
    }

    /**
     * @brief Removes a component from an entity.
     *
     * If the component pool or component does not exist, the operation has no
     * effect.
     *
     * @tparam T Component type.
     * @param entity Entity losing the component.
     *
     * @pre @p entity is valid.
     */
    template<typename T>
    void remove(Entity entity) {
        assert(valid(entity) && "Cannot remove a component from an invalid entity");

        if (auto *pool = findPool<T>()) {
            pool->remove(entity);
        }
    }

    /**
     * @brief Checks whether an entity owns a component.
     *
     * @tparam T Component type.
     * @param entity Entity to inspect.
     * @return true if the entity is valid and owns a component of type @p T.
     */
    template<typename T>
    [[nodiscard]]
    bool has(Entity entity) const {
        if (!valid(entity)) {
            return false;
        }

        const auto *pool = findPool<T>();
        return pool != nullptr && pool->has(entity);
    }

    /**
     * @brief Returns a mutable component assigned to an entity.
     *
     * @tparam T Component type.
     * @param entity Entity owning the component.
     * @return Mutable reference to the component.
     *
     * @pre @p entity is valid.
     * @pre A pool for @p T exists and the entity owns the component.
     */
    template<typename T>
    T &get(Entity entity) {
        assert(valid(entity) && "Cannot get a component from an invalid entity");

        auto *pool = findPool<T>();
        assert(pool != nullptr && "Component type is not registered");
        return pool->get(entity);
    }

    /**
     * @brief Returns a read-only component assigned to an entity.
     *
     * @tparam T Component type.
     * @param entity Entity owning the component.
     * @return Constant reference to the component.
     *
     * @pre @p entity is valid.
     * @pre A pool for @p T exists and the entity owns the component.
     */
    template<typename T>
    const T &get(Entity entity) const {
        assert(valid(entity) && "Cannot get a component from an invalid entity");

        const auto *pool = findPool<T>();
        assert(pool != nullptr && "Component type is not registered");
        return pool->get(entity);
    }

    /**
     * @brief Invokes a function for entities matching a component set.
     *
     * When component types are provided, iteration uses the dense entity list
     * of the first component pool and checks that every remaining component is
     * present. With an empty component pack, the function is invoked once for
     * every live entity.
     *
     * Callback signatures:
     * @code
     * void(Entity);
     * void(Entity, Components&...);
     * @endcode
     *
     * @tparam Components Required component types.
     * @tparam Func Callable type.
     * @param func Function invoked for each matching entity.
     *
     * @note Put the least common component first to reduce iteration work.
     */
    template<typename... Components, typename Func>
    void view(Func &&func) {
        if constexpr (sizeof...(Components) == 0) {
            for (const Entity entity : m_entities) {
                std::invoke(func, entity);
            }
        } else {
            using FirstComponent =
                std::tuple_element_t<0, std::tuple<Components...>>;
            auto *firstPool = findPool<FirstComponent>();

            if (firstPool == nullptr) {
                return;
            }

            for (const Entity entity : firstPool->entities()) {
                if ((has<Components>(entity) && ...)) {
                    std::invoke(func, entity, get<Components>(entity)...);
                }
            }
        }
    }

    /**
     * @brief Invokes a function for entities matching a component set.
     *
     * This overload permits read-only systems, such as scene serialization,
     * to iterate a const registry. Component arguments passed to the callback
     * are const references.
     */
    template<typename... Components, typename Func>
    void view(Func &&func) const {
        if constexpr (sizeof...(Components) == 0) {
            for (const Entity entity : m_entities) {
                std::invoke(func, entity);
            }
        } else {
            using FirstComponent =
                std::tuple_element_t<0, std::tuple<Components...>>;
            const auto *firstPool = findPool<FirstComponent>();

            if (firstPool == nullptr) {
                return;
            }

            for (const Entity entity : firstPool->entities()) {
                if ((has<Components>(entity) && ...)) {
                    std::invoke(func, entity, get<Components>(entity)...);
                }
            }
        }
    }

private:
    /**
     * @brief Returns the pool for a component type, creating it when needed.
     *
     * @tparam T Component type.
     * @return Reference to the component pool.
     */
    template<typename T>
    ComponentPool<T> &getOrCreatePool() {
        const std::type_index type = typeid(T);
        const auto it = m_componentPools.try_emplace(
            type,
            std::make_unique<ComponentPool<T>>()).first;

        return static_cast<ComponentPool<T> &>(*it->second);
    }

    /**
     * @brief Finds a mutable component pool.
     *
     * @tparam T Component type.
     * @return Pointer to the pool, or nullptr if it has not been created.
     */
    template<typename T>
    ComponentPool<T> *findPool() {
        const auto it = m_componentPools.find(typeid(T));
        return it == m_componentPools.end()
            ? nullptr
            : static_cast<ComponentPool<T> *>(it->second.get());
    }

    /**
     * @brief Finds a read-only component pool.
     *
     * @tparam T Component type.
     * @return Pointer to the pool, or nullptr if it has not been created.
     */
    template<typename T>
    const ComponentPool<T> *findPool() const {
        const auto it = m_componentPools.find(typeid(T));
        return it == m_componentPools.end()
            ? nullptr
            : static_cast<const ComponentPool<T> *>(it->second.get());
    }

private:
    /**
     * @brief Identifier that will be assigned to the next created entity.
     */
    Entity m_nextEntity = 1;

    /**
     * @brief Set of currently alive entities.
     */
    std::unordered_set<Entity> m_entities;

    /**
     * @brief Type-erased component pools indexed by component type.
     */
    std::unordered_map<
        std::type_index,
        std::unique_ptr<IComponentPool>
    > m_componentPools;
};

}
