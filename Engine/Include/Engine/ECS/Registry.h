#pragma once

#include "Entity.h"
#include "Componentpool.h"

#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <typeindex>
#include <memory>
#include <cassert>
#include <deque>
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
                m_entityPositions.push_back(InvalidEntityIndex);
            }
            const Entity entity = makeEntity(index, m_generations[index]);
            m_entityPositions[index] = static_cast<std::uint32_t>(m_entities.size());
            m_entities.push_back(entity);
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
            if (!valid(entity)) {
                return;
            }

            for (auto &pool: m_componentPools | std::views::values) {
                pool->remove(entity);
            }
            for (auto& changes : m_componentChangeEntities | std::views::values) {
                changes.erase(entity);
            }
            const std::uint32_t index = entityIndex(entity);
            const std::uint32_t denseIndex = m_entityPositions[index];
            const Entity movedEntity = m_entities.back();
            m_entities[denseIndex] = movedEntity;
            m_entityPositions[entityIndex(movedEntity)] = denseIndex;
            m_entities.pop_back();
            m_entityPositions[index] = InvalidEntityIndex;
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
                for (auto &pool: m_componentPools | std::views::values) {
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
            const std::uint32_t index = entityIndex(entity);
            return entity != NullEntity && index < m_entityPositions.size() &&
                   m_entityPositions[index] != InvalidEntityIndex &&
                   m_entityPositions[index] < m_entities.size() &&
                   m_entities[m_entityPositions[index]] == entity;
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
            const auto it = m_componentRevisions.find(typeid(T)); //NOLINT
            return it == m_componentRevisions.end() ? 0 : it->second;
        }

        /**
         * Returns distinct entities whose component changed after the supplied
         * revision. Recent revisions are served from an append-only delta log;
         * an old observer transparently falls back to the current-state map.
         */
        template<typename T>
        [[nodiscard]] std::vector<Entity> componentEntitiesChangedSince(
            const std::uint64_t revision) const {
            const auto type = std::type_index(typeid(T)); //NOLINT
            const auto current = m_componentChangeEntities.find(type);
            if (current == m_componentChangeEntities.end()) {
                return {};
            }

            std::vector<Entity> changed;
            const auto log = m_componentChangeLogs.find(type);
            if (log != m_componentChangeLogs.end() && !log->second.records.empty() &&
                revision >= log->second.firstRevision - 1) {
                std::unordered_set<Entity> unique;
                unique.reserve(log->second.records.size());
                for (const ComponentChange& entry : log->second.records) {
                    if (entry.revision > revision) unique.insert(entry.entity);
                }
                changed.assign(unique.begin(), unique.end());
                return changed;
            }

            // The observer predates the bounded delta log. This slower path
            // remains correct and is used only after a long pause.
            changed.reserve(current->second.size());
            for (const auto& [entity, changedRevision] : current->second) {
                if (changedRevision > revision) changed.push_back(entity);
            }
            return changed;
        }

        /**
         * Visits component changes after @p revision without allocating a result
         * vector. The recent-log path may yield the same entity repeatedly.
         */
        template<typename T, typename Func>
        void forEachComponentChangedSince(const std::uint64_t revision, Func &&func) const {
            const auto type = std::type_index(typeid(T)); //NOLINT
            const auto current = m_componentChangeEntities.find(type);
            if (current == m_componentChangeEntities.end()) return;

            const auto log = m_componentChangeLogs.find(type);
            if (log != m_componentChangeLogs.end() && !log->second.records.empty() &&
                revision >= log->second.firstRevision - 1) {
                for (const ComponentChange &entry : log->second.records) {
                    if (entry.revision > revision) std::invoke(func, entry.entity);
                }
                return;
            }
            for (const auto &[entity, changedRevision] : current->second) {
                if (changedRevision > revision) std::invoke(func, entity);
            }
        }

        /** Number of sparse entity slots, suitable for index-addressed caches. */
        [[nodiscard]] std::size_t entityIndexCapacity() const noexcept {
            return m_entityPositions.size();
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
            if (!valid(entity)) {
                throw std::invalid_argument("Cannot add a component to an invalid entity");
            }
            if (has<T>(entity)) {
                throw std::logic_error("Component already exists for this entity");
            }
            T &component = getOrCreatePool<T>().add(entity, std::forward<Args>(args)...);
            ++m_mutationRevision;
            ++m_structuralRevision;
            bumpComponentRevision<T>(entity);
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
            if (!valid(entity)) {
                throw std::invalid_argument("Cannot remove a component from an invalid entity");
            }

            if (auto *pool = findPool<T>(); pool != nullptr && pool->has(entity)) {
                pool->remove(entity);
                ++m_mutationRevision;
                ++m_structuralRevision;
                bumpComponentRevision<T>(entity);
            }
        }

        /** Marks a component changed after modifying a retained reference. */
        template<typename T>
        void markChanged(Entity entity) {
            if (!has<T>(entity)) {
                throw std::out_of_range("Cannot mark a missing component as changed");
            }
            ++m_mutationRevision;
            bumpComponentRevision<T>(entity);
        }

        /** Applies a mutation and records it for systems observing this component. */
        template<typename T, typename Func>
        void modify(Entity entity, Func &&func) {
            if (!has<T>(entity)) {
                throw std::out_of_range("Cannot modify a missing component");
            }
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
            if (!valid(entity)) {
                throw std::out_of_range("Cannot get a component from an invalid entity");
            }

            auto *pool = findPool<T>();
            if (pool == nullptr || !pool->has(entity)) {
                throw std::out_of_range(
                    "Component does not exist for this entity");
            }
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
            if (!valid(entity)) {
                throw std::out_of_range("Cannot get a component from an invalid entity");
            }

            const auto *pool = findPool<T>();
            if (pool == nullptr || !pool->has(entity)) {
                throw std::out_of_range(
                    "Component does not exist for this entity");
            }
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
                for (const Entity entity: m_entities) {
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
                for (const Entity entity: m_entities) {
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
            explicit ViewIterationGuard(Registry &registry) noexcept : registry_(registry) {
                ++registry_.m_mutableViewDepth;
            }

            ~ViewIterationGuard() { --registry_.m_mutableViewDepth; }

        private:
            Registry &registry_;
        };

        void ensureStructuralMutationAllowed() const {
            if (m_mutableViewDepth != 0) {
                throw std::logic_error("Cannot change ECS structure while iterating a mutable Registry view");
            }
        }

        template<typename T>
        void bumpComponentRevision(const Entity entity) {
            const auto type = std::type_index(typeid(T));
            const auto revision = ++m_componentRevisions[type];
            m_componentChangeEntities[type][entity] = revision;
            ComponentChangeLog& log = m_componentChangeLogs[type];
            log.records.push_back({entity, revision});
            if (log.records.size() > MaxComponentChangeLogSize) log.records.pop_front();
            log.firstRevision = log.records.front().revision;
        }

        template<typename Pools>
        [[nodiscard]] static bool allPoolsExist(const Pools &pools) {
            return std::apply([](auto *... pool) {
                return ((pool != nullptr) && ...);
            }, pools);
        }

        template<std::size_t Index = 0, typename Pools, typename Func>
        static void forEachPoolAt(std::size_t targetIndex, const Pools &pools,
                                  Func &&func) {
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
        static void forEachSmallestPool(const Pools &pools, Func &&func) {
            std::size_t smallestIndex = 0;
            std::size_t smallestSize = std::get<0>(pools)->size();

            std::apply([&](auto *... pool) {
                std::size_t index = 0;
                ((pool->size() < smallestSize
                      ? (smallestSize = pool->size(), smallestIndex = index)
                      : 0,
                  ++index), ...);
            }, pools);

            forEachPoolAt(smallestIndex, pools, [&](const auto &drivingPool) {
                for (const Entity entity: drivingPool.entities()) {
                    if (std::apply([entity](auto *... pool) {
                        return (pool->has(entity) && ...);
                    }, pools)) {
                        std::apply([&](auto *... pool) {
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
            const auto it = m_componentPools.try_emplace( //NOLINT
                type,
                std::make_unique<ComponentPool<T> >()).first;

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
            const auto it = m_componentPools.find(typeid(T)); //NOLINT
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
            const auto it = m_componentPools.find(typeid(T)); //NOLINT
            return it == m_componentPools.end()
                       ? nullptr
                       : static_cast<const ComponentPool<T> *>(it->second.get());
        }

        /**
         * @brief Identifier that will be assigned to the next created entity.
         */
        std::uint32_t m_nextEntity = 1;

        /** Generation counters prevent stale handles becoming valid after reuse. */
        std::vector<std::uint32_t> m_generations{0};

        /** Reusable sparse-set indices released by destroy(). */
        std::vector<std::uint32_t> m_freeEntities;

        static constexpr std::uint32_t InvalidEntityIndex =
            std::numeric_limits<std::uint32_t>::max();

        /** Dense live-entity list and sparse index for O(1) validation/removal. */
        std::vector<Entity> m_entities;
        std::vector<std::uint32_t> m_entityPositions{InvalidEntityIndex};

        /**
         * @brief Type-erased component pools indexed by component type.
         */
        std::unordered_map<
            std::type_index,
            std::unique_ptr<IComponentPool>
        > m_componentPools;

        std::unordered_map<std::type_index, std::uint64_t> m_componentRevisions;
        struct ComponentChange {
            Entity entity{NullEntity};
            std::uint64_t revision{0};
        };
        struct ComponentChangeLog {
            std::uint64_t firstRevision{0};
            std::deque<ComponentChange> records;
        };
        static constexpr std::size_t MaxComponentChangeLogSize = 4096;
        std::unordered_map<std::type_index, std::unordered_map<Entity, std::uint64_t> >
        m_componentChangeEntities;
        std::unordered_map<std::type_index, ComponentChangeLog> m_componentChangeLogs;

        std::uint64_t m_mutationRevision = 0;
        std::uint64_t m_structuralRevision = 0;
        std::uint32_t m_mutableViewDepth = 0;
    };
}
