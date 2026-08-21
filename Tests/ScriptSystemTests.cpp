#include <Engine/Core/Transform.h>
#include <Engine/ECS/Components/ScriptComponent.h>
#include <Engine/ECS/Registry.h>
#include <Engine/Scripting/ScriptSystem.h>

namespace {

class MoveRight final : public Engine::Script {
public:
    void onCreate() override { ++created; }
    void onUpdate(const float deltaTime) override {
        ++updated;
        transform().position.setX(transform().position.x() + deltaTime);
    }

    inline static int created{};
    inline static int updated{};
};

} // namespace

int main() {
    using namespace Engine;
    Registry registry;
    const Entity entity = registry.create();
    registry.add<Transform>(entity);
    registry.add<ScriptComponent>(entity, ScriptComponent{"MoveRight"});

    ScriptRegistry scriptRegistry;
    scriptRegistry.registerClass<MoveRight>("MoveRight");
    ScriptSystem scripts{scriptRegistry};
    scripts.update(registry, 0.25f);
    scripts.update(registry, 0.75f);

    if (MoveRight::created != 1 || MoveRight::updated != 2 ||
        registry.get<Transform>(entity).position.x() != 1.0f) return 1;

    registry.modify<ScriptComponent>(entity, [](auto& script) { script.enabled = false; });
    scripts.update(registry, 1.0f);
    return MoveRight::updated == 2 ? 0 : 2;
}
