#include "Engine/Scripting/Script.h"

#include "Engine/ECS/Actor.h"
#include "Engine/Scene/Scene.h"

#include <stdexcept>

namespace Engine {

Actor Script::actor() const {
    if (scene_ == nullptr || registry_ == nullptr || entity_ == NullEntity) {
        throw std::logic_error("Script is not attached to a live actor");
    }
    const auto* object = scene_->findByEntity(entity_);
    if (object == nullptr) { throw std::logic_error("Script actor no longer exists"); }
    return Actor{*scene_, object->objectId()};
}

Scene& Script::scene() const {
    if (scene_ == nullptr) { throw std::logic_error("Script is not attached to a scene"); }
    return *scene_;
}

} // namespace Engine
