#pragma once

#include "Entity.h"

#include <cassert>
#include <limits>
#include <utility>
#include <vector>

namespace Engine {

/**
 * @brief Type-erased interface for an ECS component pool.
 *
 * The registry stores component pools through this interface so that it can
 * remove components without knowing their concrete component type.
 */
class IComponentPool {
public:
    /**
     * @brief Destroys the component pool.
     */
    virtual ~IComponentPool() = default;

    /**
     * @brief Removes a component assigned to an entity.
     *
     * Calling this function for an entity without the component has no effect.
     *
     * @param entity Entity whose component should be removed.
     */
    virtual void remove(Entity entity) = 0;

    /**
     * @brief Checks whether an entity owns a component in this pool.
     *
     * @param entity Entity to check.
     * @return true if the component exists; otherwise false.
     */
    [[nodiscard]] virtual bool has(Entity entity) const = 0;
};

/**
 * @brief Sparse-set storage for components of one concrete type.
 *
 * Components and entity identifiers are stored in dense arrays for efficient
 * iteration. A sparse array maps an entity identifier to its dense-array index.
 * Removal uses swap-and-pop, so component order is not stable.
 *
 * @tparam T Component type stored by the pool.
 */
template<typename T>
class ComponentPool final : public IComponentPool {
public:
    /**
     * @brief Constructs and adds a component for an entity.
     *
     * The entity must not already own a component of type @p T.
     *
     * @tparam Args Constructor argument types.
     * @param entity Entity receiving the component.
     * @param args Arguments forwarded to the component constructor.
     * @return Reference to the newly created component.
     */
    template<typename... Args>
    T &add(Entity entity, Args &&... args) {
        assert(!has(entity) && "Component already exists for this entity");
        if (has(entity)) {
            return get(entity);
        }

        ensureSparseCapacity(entity);
        const auto index = static_cast<std::uint32_t>(m_components.size());
        m_components.emplace_back(std::forward<Args>(args)...);
        try {
            m_entities.push_back(entity);
        } catch (...) {
            m_components.pop_back();
            throw;
        }
        m_sparse[entityIndex(entity)] = index;
        return m_components.back();
    }

    /**
     * @brief Removes the component associated with an entity.
     *
     * Uses swap-and-pop to keep component data densely packed. References,
     * pointers, and iterators to components may be invalidated.
     *
     * @param entity Entity whose component should be removed.
     */
    void remove(Entity entity) override {
        if (!has(entity)) {
            return;
        }

        const auto index = m_sparse[entityIndex(entity)];
        const auto lastIndex = m_components.size() - 1;

        if (index != lastIndex) {
            m_components[index] = std::move(m_components[lastIndex]);

            const Entity movedEntity = m_entities[lastIndex];
            m_entities[index] = movedEntity;
            m_sparse[entityIndex(movedEntity)] = index;
        }

        m_components.pop_back();
        m_entities.pop_back();
        m_sparse[entityIndex(entity)] = InvalidIndex;
    }

    /**
     * @brief Checks whether an entity owns a component in this pool.
     *
     * @param entity Entity to check.
     * @return true if the entity has a component of type @p T.
     */
    [[nodiscard]] bool has(Entity entity) const override {
        const auto index = Engine::entityIndex(entity);
        return index < m_sparse.size()
            && m_sparse[index] != InvalidIndex
            && m_sparse[index] < m_entities.size()
            && m_entities[m_sparse[index]] == entity;
    }

    /**
     * @brief Returns a mutable component assigned to an entity.
     *
     * @param entity Entity owning the component.
     * @return Mutable reference to the component.
     *
     * @pre The entity has a component of type @p T.
     */
    T& get(Entity entity) {
        assert(has(entity) && "Component does not exist for this entity");
        return m_components[m_sparse[entityIndex(entity)]];
    }

    /**
     * @brief Returns a read-only component assigned to an entity.
     *
     * @param entity Entity owning the component.
     * @return Constant reference to the component.
     *
     * @pre The entity has a component of type @p T.
     */
    const T& get(Entity entity) const {
        assert(has(entity) && "Component does not exist for this entity");
        return m_components[m_sparse[entityIndex(entity)]];
    }

    /**
     * @brief Returns the dense list of entities stored in this pool.
     *
     * The entity at index @c i owns the component at index @c i in the dense
     * component array.
     *
     * @return Read-only reference to the entity array.
     */
    [[nodiscard]] const std::vector<Entity>& entities() const {
        return m_entities;
    }

    /**
     * @brief Returns the number of components stored in the dense array.
     */
    [[nodiscard]] std::size_t size() const noexcept {
        return m_entities.size();
    }

    /**
     * @brief Returns a component without repeating the membership check.
     *
     * @pre The entity owns a component in this pool.
     */
    T& getUnchecked(Entity entity) noexcept {
        return m_components[m_sparse[entityIndex(entity)]];
    }

    /**
     * @brief Returns a component without repeating the membership check.
     *
     * @pre The entity owns a component in this pool.
     */
    const T& getUnchecked(Entity entity) const noexcept {
        return m_components[m_sparse[entityIndex(entity)]];
    }

private:
    /**
     * @brief Sentinel indicating that an entity has no dense-array entry.
     */
    static constexpr std::uint32_t InvalidIndex =
        std::numeric_limits<std::uint32_t>::max();

    /**
     * @brief Enlarges the sparse array so it can index an entity.
     *
     * Newly created entries are initialized with @ref InvalidIndex.
     *
     * @param entity Entity whose numeric value must become addressable.
     */
    void ensureSparseCapacity(Entity entity) {
        const auto index = Engine::entityIndex(entity);

        if (index >= m_sparse.size()) {
            m_sparse.resize(static_cast<std::size_t>(index) + 1, InvalidIndex);
        }
    }

    /**
     * @brief Densely packed component storage.
     */
    std::vector<T> m_components;

    /**
     * @brief Entity identifiers corresponding to entries in m_components.
     */
    std::vector<Entity> m_entities;

    /**
     * @brief Sparse mapping from entity identifier to dense-array index.
     */
    // Entity indices are uint32_t, so size_t only doubles this table on
    // 64-bit platforms without providing additional addressable entities.
    std::vector<std::uint32_t> m_sparse;
};

}
