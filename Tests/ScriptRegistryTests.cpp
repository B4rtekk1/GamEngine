#include <Engine/Core/Transform.h>
#include <Engine/ECS/Components/ScriptComponent.h>
#include <Engine/ECS/Registry.h>
#include <Engine/Scripting/Script.h>
#include <Engine/Scripting/ScriptRegistry.h>
#include <Engine/Scripting/ScriptSystem.h>

#include <algorithm>

namespace {
class FirstScript final : public Engine::Script {
public:
    void onCreate() override { ++created; }
    void onUpdate(float deltaTime) override { lastDelta = deltaTime; ++updated; }
    void onDestroy() override { ++destroyed; }

    inline static int created{};
    inline static int updated{};
    inline static int destroyed{};
    inline static float lastDelta{};
};

class SecondScript final : public Engine::Script {
public:
    void onCreate() override { ++created; }
    void onUpdate(float) override { ++updated; }

    inline static int created{};
    inline static int updated{};
};
}

int main() {
    using namespace Engine;

    ScriptRegistry registry;
    registry.registerClass<FirstScript>("First");
    registry.registerClass<SecondScript>("Second");
    if (!registry.className<FirstScript>() || *registry.className<FirstScript>() != "First" ||
        registry.className<Script>().has_value() || registry.create("Missing") != nullptr) return 1;
    const auto names = registry.classNames();
    if (names.size() != 2 || std::find(names.begin(), names.end(), "First") == names.end() ||
        std::find(names.begin(), names.end(), "Second") == names.end()) return 2;
    if (dynamic_cast<FirstScript*>(registry.create("First").get()) == nullptr) return 3;

    Registry entities;
    const Entity entity = entities.create();
    entities.add<Transform>(entity);
    entities.add<ScriptComponent>(entity, ScriptComponent{"First"});
    ScriptSystem system{registry};
    system.update(entities, 0.25f);
    if (FirstScript::created != 1 || FirstScript::updated != 1 ||
        FirstScript::lastDelta != 0.25f) return 4;

    entities.modify<ScriptComponent>(entity, [](auto& component) { component.className = "Second"; });
    system.update(entities, 0.5f);
    if (FirstScript::destroyed != 1 || SecondScript::created != 1 || SecondScript::updated != 1) return 5;

    entities.modify<ScriptComponent>(entity, [](auto& component) { component.enabled = false; });
    system.update(entities, 1.0f);
    if (SecondScript::updated != 1) return 6;
    entities.modify<ScriptComponent>(entity, [](auto& component) {
        component.enabled = true;
        component.className.clear();
    });
    system.update(entities, 1.0f);
    if (SecondScript::updated != 1) return 7;

    return 0;
}
