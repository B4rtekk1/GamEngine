#pragma once

#include "Engine/ECS/Entity.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

namespace Engine {
    /** Stable, serializable identifier for an object across scene saves. */
    using UUID = std::uint64_t;

    constexpr UUID NullUUID = 0;

    /** Human-readable object name shown by tools such as the scene hierarchy. */
    struct NameComponent final {
        std::string value{"GameObject"};
    };

    /** Persistent object identifier. It is independent of the recyclable Entity id. */
    struct UUIDComponent final {
        UUID value{NullUUID};
    };

    /** Persistent UUID plus runtime-only resolved parent entity. */
    struct ParentComponent final {
        UUID parentUuid{NullUUID};
        Entity runtimeParent{NullEntity};
    };

    /** Display position among objects with the same parent in the editor hierarchy. */
    struct HierarchyOrderComponent final {
        std::uint32_t value{};
    };

    inline std::atomic<UUID> &uuidCounter() noexcept {
        static std::atomic<UUID> next{1};
        return next;
    }

    /** Allocates a process-unique UUID suitable for a newly created scene object. */
    inline UUID createUUID() noexcept {
        return uuidCounter().fetch_add(1, std::memory_order_relaxed);
    }

    /** Ensures that UUIDs generated after loading cannot collide with @p value. */
    inline void reserveUUID(const UUID value) noexcept {
        if (value == NullUUID) {
            return;
        }
        std::atomic<UUID> &next = uuidCounter();
        UUID expected = next.load(std::memory_order_relaxed);
        while (expected <= value &&
               !next.compare_exchange_weak(expected, value + 1, std::memory_order_relaxed)) {
        }
    }
} // namespace Engine
