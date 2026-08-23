#include "Engine/Scripting/ScriptRegistry.h"

#include "Engine/Scripting/Script.h"

namespace Engine {

std::unique_ptr<Script> ScriptRegistry::create(const std::string_view name) const {
    const auto found = factories_.find(std::string{name});
    return found == factories_.end() ? nullptr : found->second();
}

} // namespace Engine
