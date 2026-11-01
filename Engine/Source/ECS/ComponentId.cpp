#include "Engine/ECS/ComponentId.h"

#include <mutex>
#include <typeindex>
#include <unordered_map>

namespace Engine::Detail {
std::size_t componentTypeIdSlow(const std::type_info& type) {
    // One table owns identifier assignment for the whole process. Component
    // pools are created lazily, so this runs only on the first access to a
    // component type (per shared object cache) rather than per ECS call.
    static std::mutex mutex;
    static std::unordered_map<std::type_index, std::size_t> identifiers;

    const std::scoped_lock lock(mutex);
    const auto found = identifiers.find(std::type_index{type});
    if (found != identifiers.end()) {
        return found->second;
    }
    const std::size_t id = identifiers.size();
    identifiers.emplace(std::type_index{type}, id);
    return id;
}
} // namespace Engine::Detail
