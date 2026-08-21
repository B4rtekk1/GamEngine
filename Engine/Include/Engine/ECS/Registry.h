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
#include <vector>
#include <stdexcept>

namespace Engine {

/**
 * @brief Owns entities and their component pools.
 *
 * Registry provides entity lifetime management and type-safe component
 * operations. Each component type is stored in its own ComponentPool.
 *
 * Entity slots are recycled with a generation counter, so stale identifiers
 * remain invalid after their slot is reused.
 */
class Registry {
public:
    /**
     * @brief Creates a new entity.
     *
     * @return Identifier of the newly created entity.
     */
    Entity create() {
        ensureStructuralMutationAllowed();
        std::uint32_t index;
        if (!m_freeEntities.empty()) {
            index = m_freeEntities.back();
            m_freeEntities.pop_back();
        } else {
            index = m_nextEntity++;
            m_generations.push_back(0);
        }
        const Entity entity = makeEntity(index, m_generations[index]);
        m_entities.insert(entity);
        ++m_mutationRevision;
        ++m_structuralRevision;
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
        ensureStructuralMutationAllowed();
        if (!m_entities.erase(entity)) {
            return;
        }

        for (auto &pool: m_componentPools | std::views::values) {
            pool->remove(entity);
        }
        const std::uint32_t index = entityIndex(entity);
        ++m_generations[index];
        m_freeEntities.push_back(index);
        ++m_mutationRevision;
        ++m_structuralRevision;
    }

    /**
     * @brief Creates an entity with a copy of all components from @p source.
     *
     * Component pools are traversed once and components are copied directly
     * into their dense storage. Shared resources, such as mesh data, retain
     * their normal copy semantics (and therefore remain shared).
     *
     * @param source Entity to copy.
     * @return A new entity, or @ref NullEntity when @p source is invalid.
     */
    [[nodiscard]] Entity clone(Entity source) {
        if (!valid(source)) {
            return NullEntity;
        }

        const Entity target = create();
        try {
            for (auto& pool : m_componentPools | std::views::values) {
                pool->clone(source, target);
            }
        } catch (...) {
            destroy(target);
            throw;
        }
        return target;
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

    /** Returns the number of live entities. */
    [[nodiscard]] std::size_t size() const noexcept {
        return m_entities.size();
    }

    /** @brief Monotonically increasing revision for explicit ECS changes. */
    [[nodiscard]] std::uint64_t mutationRevision() const noexcept {
        return m_mutationRevision;
    }

    /** @brief Revision for changes that invalidate renderer component pools. */
    [[nodiscard]] std::uint64_t structuralRevision() const noexcept {
        return m_structuralRevision;
    }

    /** Returns the revision of one concrete component type. */
    template<typename T>
    [[nodiscard]] std::uint64_t componentRevision() const noexcept {
        const auto it = m_componentRevisions.find(typeid(T));
        return it == m_componentRevisions.end() ? 0 : it->second;
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
        ensureStructuralMutationAllowed();
        if (!valid(entity)) throw std::invalid_argument("Cannot add a component to an invalid entity");
        if (has<T>(entity)) throw std::logic_error("Component already exists for this entity");
        T& component = getOrCreatePool<T>().add(entity, std::forward<Args>(args)...);
        ++m_mutationRevision;
        ++m_structuralRevision;
        bumpComponentRevision<T>();
        return component;
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
        ensureStructuralMutationAllowed();
        if (!valid(entity)) throw std::invalid_argument("Cannot remove a component from an invalid entity");

        if (auto *pool = findPool<T>(); pool != nullptr && pool->has(entity)) {
            pool->remove(entity);
            ++m_mutationRevision;
            ++m_structuralRevision;
            bumpComponentRevision<T>();
        }
    }

    /** Marks a component changed after modifying a retained reference. */
    template<typename T>
    void markChanged(Entity entity) {
        if (!has<T>(entity)) throw std::out_of_range("Cannot mark a missing component as changed");
        ++m_mutationRevision;
        bumpComponentRevision<T>();
    }

    /** Applies a mutation and records it for systems observing this component. */
    template<typename T, typename Func>
    void modify(Entity entity, Func&& func) {
        if (!has<T>(entity)) throw std::out_of_range("Cannot modify a missing component");
        std::invoke(std::forward<Func>(func), get<T>(entity));
        markChanged<T>(entity);
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
        if (!valid(entity)) throw std::out_of_range("Cannot get a component from an invalid entity");

        auto *pool = findPool<T>();
        if (pool == nullptr || !pool->has(entity)) throw std::out_of_range("Component does not exist for this entity");
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
        if (!valid(entity)) throw std::out_of_range("Cannot get a component from an invalid entity");

        const auto *pool = findPool<T>();
        if (pool == nullptr || !pool->has(entity)) throw std::out_of_range("Component does not exist for this entity");
        return pool->get(entity);
    }

    /**
     * @brief Invokes a function for entities matching a component set.
     *
     * When component types are provided, iteration uses the dense entity list
     * of the smallest component pool and checks membership in the remaining
     * cached pools. With an empty component pack, the function is invoked once
     * for every live entity.
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
     */
    template<typename... Components, typename Func>
    void view(Func &&func) {
        ViewIterationGuard guard{*this};
        if constexpr (sizeof...(Components) == 0) {
            for (const Entity entity : m_entities) {
                std::invoke(func, entity);
            }
        } else {
            const auto pools = std::tuple{findPool<Components>()...};
            if (!allPoolsExist(pools)) {
                return;
            }

            forEachSmallestPool(pools, std::forward<Func>(func));
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
            const auto pools = std::tuple{findPool<Components>()...};
            if (!allPoolsExist(pools)) {
                return;
            }

            forEachSmallestPool(pools, std::forward<Func>(func));
        }
    }

private:
    class ViewIterationGuard {
    public:
        explicit ViewIterationGuard(Registry& registry) noexcept : registry_(registry) {
            ++registry_.m_mutableViewDepth;
        }
        ~ViewIterationGuard() { --registry_.m_mutableViewDepth; }
    private:
        Registry& registry_;
    };

    void ensureStructuralMutationAllowed() const {
        if (m_mutableViewDepth != 0) {
            throw std::logic_error("Cannot change ECS structure while iterating a mutable Registry view");
        }
    }

    template<typename T>
    void bumpComponentRevision() {
        ++m_componentRevisions[std::type_index(typeid(T))];
    }

    template<typename Pools>
    [[nodiscard]] static bool allPoolsExist(const Pools& pools) {
        return std::apply([](auto*... pool) {
            return ((pool != nullptr) && ...);
        }, pools);
    }

    template<std::size_t Index = 0, typename Pools, typename Func>
    static void forEachPoolAt(std::size_t targetIndex, const Pools& pools,
                              Func&& func) {
        if constexpr (Index < std::tuple_size_v<Pools>) {
            if (targetIndex == Index) {
                std::forward<Func>(func)(*std::get<Index>(pools));
                return;
            }
            forEachPoolAt<Index + 1>(targetIndex, pools,
                                     std::forward<Func>(func));
        }
    }

    template<typename Pools, typename Func>
    static void forEachSmallestPool(const Pools& pools, Func&& func) {
        std::size_t smallestIndex = 0;
        std::size_t smallestSize = std::get<0>(pools)->size();

        std::apply([&](auto*... pool) {
            std::size_t index = 0;
            ((pool->size() < smallestSize
                  ? (smallestSize = pool->size(), smallestIndex = index)
                  : 0,
              ++index), ...);
        }, pools);

        forEachPoolAt(smallestIndex, pools, [&](const auto& drivingPool) {
            for (const Entity entity : drivingPool.entities()) {
                if (std::apply([entity](auto*... pool) {
                        return (pool->has(entity) && ...);
                    }, pools)) {
                    std::apply([&](auto*... pool) {
                        std::invoke(func, entity, pool->getUnchecked(entity)...);
                    }, pools);
                }
            }
        });
    }

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
    std::uint32_t m_nextEntity = 1;

    /** Generation counters prevent stale handles becoming valid after reuse. */
    std::vector<std::uint32_t> m_generations{0};

    /** Reusable sparse-set indices released by destroy(). */
    std::vector<std::uint32_t> m_freeEntities;

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

    std::unordered_map<std::type_index, std::uint64_t> m_componentRevisions;

    std::uint64_t m_mutationRevision = 0;
    std::uint64_t m_structuralRevision = 0;
    std::uint32_t m_mutableViewDepth = 0;
};

}
