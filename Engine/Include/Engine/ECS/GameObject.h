#pragma once

#include "Engine/ECS/Components/MeshRendererComponent.h"
#include "Engine/ECS/Components/TransformComponent.h"
#include "Engine/ECS/Entity.h"
#include "Engine/ECS/Registry.h"

#include <cassert>
#include <atomic>
#include <memory>
#include <utility>

namespace Engine {

    /**
     * @brief Object-oriented handle for an entity in a Registry.
     *
     * GameObject owns the lifetime of the entity it creates, while the
     * component data remains owned by the Registry. A spawned object always
     * has TransformComponent and MeshRendererComponent; other ECS components
     * can be attached through add(), just like on any other entity.
     *
     * The Registry must outlive the GameObject.
     */
    class GameObject {
    public:
        explicit GameObject(Registry& registry) noexcept
            : m_registry(&registry),
              m_objectId(nextObjectId()) {
        }

        GameObject(const GameObject&) = delete;
        GameObject& operator=(const GameObject&) = delete;

        GameObject(GameObject&& other) noexcept
            : m_registry(other.m_registry),
              m_objectId(std::exchange(other.m_objectId, NullObjectId)),
              m_entity(std::exchange(other.m_entity, NullEntity)),
              m_spawned(std::exchange(other.m_spawned, false)),
              m_isClone(std::exchange(other.m_isClone, false)) {
        }

        GameObject& operator=(GameObject&& other) noexcept {
            if (this == &other) {
                return *this;
            }

            if (m_isClone) {
                OnCloneDestroy();
            }
            releaseEntity();
            m_registry = other.m_registry;
            m_objectId = std::exchange(other.m_objectId, NullObjectId);
            m_entity = std::exchange(other.m_entity, NullEntity);
            m_spawned = std::exchange(other.m_spawned, false);
            m_isClone = std::exchange(other.m_isClone, false);
            return *this;
        }

        virtual ~GameObject() {
            // Destruction from the C++ destructor does not invoke OnDestroy;
            // explicit destroy() is the lifecycle-aware operation.
            if (m_isClone) {
                GameObject::OnCloneDestroy();
            }
            releaseEntity();
        }

        /** @brief Creates the entity and its required base components. */
        void spawn() {
            if (m_spawned) {
                return;
            }

            m_entity = m_registry->create();
            m_registry->add<TransformComponent>(m_entity);
            m_registry->add<MeshRendererComponent>(m_entity);
            m_spawned = true;

            OnSpawn();
        }

        /** @brief Removes the entity and all of its components from the registry. */
        void destroy() {
            if (!m_spawned) {
                return;
            }

            OnDestroy();
            if (m_isClone) {
                OnCloneDestroy();
            }
            releaseEntity();
        }

        [[nodiscard]] bool isSpawned() const noexcept {
            return m_spawned && m_registry->valid(m_entity);
        }

        /** @brief Returns this object's entity identifier. */
        [[nodiscard]] Entity entity() const noexcept {
            return m_entity;
        }

        /** @brief Returns this object's stable identifier. */
        [[nodiscard]] ObjectId objectId() const noexcept {
            return m_objectId;
        }

        /**
         * @brief Creates an independent GameObject with copied components.
         *
         * The clone uses the same Registry and receives copies of every
         * component attached to this object. Mesh resources remain shared via
         * their std::shared_ptr, while per-object state is copied.
         * An unspawned object produces another unspawned object.
         */
        [[nodiscard]] GameObject clone() const {
            GameObject result(*m_registry);
            if (!isSpawned()) {
                return result;
            }

            result.m_entity = m_registry->clone(m_entity);
            result.m_spawned = result.m_entity != NullEntity;
            result.m_isClone = result.m_spawned;
            if (result.m_isClone) {
                result.OnClone();
            }
            return result;
        }

        /** @brief Returns the registry containing this object's entity. */
        [[nodiscard]] Registry& registry() noexcept {
            return *m_registry;
        }

        [[nodiscard]] const Registry& registry() const noexcept {
            return *m_registry;
        }

        template<typename T, typename... Args>
        T& add(Args&&... args) {
            assert(isSpawned() && "Cannot add a component to an unspawned GameObject");
            return m_registry->add<T>(m_entity, std::forward<Args>(args)...);
        }

        template<typename T>
        void remove() {
            assert(isSpawned() && "Cannot remove a component from an unspawned GameObject");
            m_registry->remove<T>(m_entity);
        }

        template<typename T>
        [[nodiscard]] bool has() const {
            return isSpawned() && m_registry->has<T>(m_entity);
        }

        template<typename T>
        T& get() {
            assert(isSpawned() && "Cannot get a component from an unspawned GameObject");
            return m_registry->get<T>(m_entity);
        }

        template<typename T>
        [[nodiscard]] const T& get() const {
            assert(isSpawned() && "Cannot get a component from an unspawned GameObject");
            return m_registry->get<T>(m_entity);
        }

        [[nodiscard]] TransformComponent& transform() {
            return get<TransformComponent>();
        }

        [[nodiscard]] const TransformComponent& transform() const {
            return get<TransformComponent>();
        }

        [[nodiscard]] MeshRendererComponent& meshRenderer() {
            return get<MeshRendererComponent>();
        }

        [[nodiscard]] const MeshRendererComponent& meshRenderer() const {
            return get<MeshRendererComponent>();
        }

        [[nodiscard]] Mat4 modelMatrix() const noexcept {
            return transform().matrix();
        }

    protected:
        virtual void OnSpawn() {}
        virtual void OnDestroy() {}

        /** @brief Called on the newly created clone after its components copy. */
        virtual void OnClone() {}

        /** @brief Called when a cloned object is being destroyed. */
        virtual void OnCloneDestroy() {}

    private:
        static ObjectId nextObjectId() noexcept {
            return s_nextObjectId.fetch_add(1, std::memory_order_relaxed);
        }

        void releaseEntity() noexcept {
            if (m_spawned && m_registry->valid(m_entity)) {
                m_registry->destroy(m_entity);
            }
            m_entity = NullEntity;
            m_spawned = false;
            m_isClone = false;
        }

        inline static std::atomic<ObjectId> s_nextObjectId{1};
        Registry* m_registry;
        ObjectId m_objectId{NullObjectId};
        Entity m_entity{NullEntity};
        bool m_spawned{false};
        bool m_isClone{false};
    };

}
