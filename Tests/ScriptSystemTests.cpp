#include <gtest/gtest.h>

#include "Engine/Core/Transform.h"
#include "Engine/ECS/Components/ScriptComponent.h"
#include "Engine/ECS/Registry.h"
#include "Engine/Scripting/Script.h"
#include "Engine/Scripting/ScriptRegistry.h"
#include "Engine/Scripting/ScriptSystem.h"
#include "Engine/Scene/Scene.h"

#include <string>

namespace {

class LifecycleTestScript final : public Engine::Script {
public:
    static inline int created = 0;
    static inline int updated = 0;
    static inline int destroyed = 0;
    static inline float lastDeltaTime = 0.0F;

    static void reset() {
        created = 0;
        updated = 0;
        destroyed = 0;
        lastDeltaTime = 0.0F;
    }

    void onCreate() override { ++created; }
    void onUpdate(const float deltaTime) override {
        ++updated;
        lastDeltaTime = deltaTime;
    }
    void onDestroy() override { ++destroyed; }
};

class SceneAwareTestScript final : public Engine::Script {
public:
    static inline std::string actorName;
    static inline bool foundInScene = false;

    static void reset() {
        actorName.clear();
        foundInScene = false;
    }

    void onCreate() override {
        actorName = actor().name();
        foundInScene = scene().findActor(actorName).valid();
    }

    void onUpdate(float) override {
        transform().position.setY(7.0F);
    }
};

TEST(ScriptSystem, ManagesRuntimeLifecycleAndTransformChanges) {
    LifecycleTestScript::reset();
    auto& scripts = Engine::ScriptRegistry::instance();
    scripts.registerClass<LifecycleTestScript>("LifecycleTestScript");
    Engine::ScriptSystem system{scripts};
    Engine::Registry registry;
    const auto entity = registry.create();
    registry.add<Engine::Transform>(entity);
    registry.add<Engine::ScriptComponent>(entity, Engine::ScriptComponent{"LifecycleTestScript"});
    const auto transformRevision = registry.componentRevision<Engine::Transform>();

    system.update(registry, 0.25F);
    auto& component = registry.get<Engine::ScriptComponent>(entity);
    EXPECT_EQ(LifecycleTestScript::created, 1);
    EXPECT_EQ(LifecycleTestScript::updated, 1);
    EXPECT_FLOAT_EQ(LifecycleTestScript::lastDeltaTime, 0.25F);
    EXPECT_GT(registry.componentRevision<Engine::Transform>(), transformRevision);

    system.update(registry, 0.5F);
    EXPECT_EQ(LifecycleTestScript::created, 1);
    EXPECT_EQ(LifecycleTestScript::updated, 2);
    EXPECT_FLOAT_EQ(LifecycleTestScript::lastDeltaTime, 0.5F);

    component.className = "UnknownScript";
    system.update(registry, 1.0F);
    EXPECT_EQ(LifecycleTestScript::destroyed, 1);
}

TEST(ScriptSystem, SkipsDisabledAndEmptyScriptComponents) {
    LifecycleTestScript::reset();
    auto& scripts = Engine::ScriptRegistry::instance();
    scripts.registerClass<LifecycleTestScript>("LifecycleTestScript");
    Engine::ScriptSystem system{scripts};
    Engine::Registry registry;
    const auto disabled = registry.create();
    registry.add<Engine::ScriptComponent>(disabled,
                                          Engine::ScriptComponent{"LifecycleTestScript", false});
    const auto empty = registry.create();
    registry.add<Engine::ScriptComponent>(empty);

    system.update(registry, 0.1F);
    EXPECT_EQ(LifecycleTestScript::created, 0);
    EXPECT_EQ(LifecycleTestScript::updated, 0);
}

TEST(ScriptSystem, AttachesSceneAwareScriptsToTheirActor) {
    SceneAwareTestScript::reset();
    auto& scripts = Engine::ScriptRegistry::instance();
    scripts.registerClass<SceneAwareTestScript>("SceneAwareTestScript");
    Engine::Scene scene;
    const auto actor = scene.createActor("Scripted actor");
    actor.addScript("SceneAwareTestScript");

    Engine::ScriptSystem system{scripts};
    system.update(scene, 1.0F / 60.0F);
    EXPECT_EQ(SceneAwareTestScript::actorName, "Scripted actor");
    EXPECT_TRUE(SceneAwareTestScript::foundInScene);
    EXPECT_FLOAT_EQ(actor.position().y(), 7.0F);
}

} // namespace
