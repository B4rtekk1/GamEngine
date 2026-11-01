#pragma once

#include <cstddef>
#include <typeinfo>

namespace Engine::Detail {
/**
 * @brief Maps a component type to a dense, process-wide identifier.
 *
 * The implementation lives in the Engine library so that every dynamically
 * loaded game-script module receives the same identifier for the same type.
 * A header-local counter would be duplicated per shared object on platforms
 * without vague-linkage merging (notably Windows), sending script-side
 * component accesses to the wrong pool.
 */
[[nodiscard]] std::size_t componentTypeIdSlow(const std::type_info& type);

/**
 * @brief Returns the stable dense index of a component type.
 *
 * Registries use this value to store per-component state in vectors instead
 * of hashing std::type_index in every get/has/change call. The type-index
 * lookup happens once per type per shared object; the returned value is
 * shared by all of them.
 */
template <typename T> [[nodiscard]] std::size_t componentTypeId() {
    static const std::size_t id = componentTypeIdSlow(typeid(T));
    return id;
}
} // namespace Engine::Detail
