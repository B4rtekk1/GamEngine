#include <gtest/gtest.h>

#include "Engine/Core/Transform.h"
#include "Engine/Core/Diagnostics.h"
#include "Engine/ECS/Components/ScriptComponent.h"
#include "Engine/ECS/Registry.h"
#include "Engine/Scripting/Script.h"
#include "Engine/Scripting/ScriptRegistry.h"
#include "Engine/Scripting/ScriptSystem.h"
#include "Engine/Scene/Scene.h"

#include <string>
#include <stdexcept>

namespace {

class LifecycleTestScript final : public Engine::Script {
public:
    static inline int created = 0;
    static inline int updated = 0;
    static inline int destroyed = 0;
    static inline int enabled = 0;
    static inline int disabled = 0;
    static inline float lastDeltaTime = 0.0F;

    static void reset() {
        created = 0;
        updated = 0;
        destroyed = 0;
        enabled = 0;
        disabled = 0;
        lastDeltaTime = 0.0F;
    }

    void onCreate() override { ++created; }
    void onUpdate(const float deltaTime) override {
        ++updated;
        lastDeltaTime = deltaTime;
    }
    void onDestroy() override { ++destroyed; }
    void onEnable() override { ++enabled; }
    void onDisable() override { ++disabled; }
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

class ThrowingTestScript final : public Engine::Script {
public:
    void onUpdate(float) override { throw std::runtime_error{"test failure"}; }
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

TEST(ScriptSystem, PreservesScriptStateAcrossDisableAndEnable) {
    LifecycleTestScript::reset();
    auto& scripts = Engine::ScriptRegistry::instance();
    scripts.registerClass<LifecycleTestScript>("EnableDisableLifecycleTestScript");
    Engine::ScriptSystem system{scripts};
    Engine::Registry registry;
    const auto entity = registry.create();
    registry.add<Engine::ScriptComponent>(entity,
                                          Engine::ScriptComponent{"EnableDisableLifecycleTestScript"});

    system.update(registry, 0.1F);
    auto& component = registry.get<Engine::ScriptComponent>(entity);
    component.enabled = false;
    system.update(registry, 0.1F);
    component.enabled = true;
    system.update(registry, 0.1F);

    EXPECT_EQ(LifecycleTestScript::created, 1);
    EXPECT_EQ(LifecycleTestScript::enabled, 2);
    EXPECT_EQ(LifecycleTestScript::disabled, 1);
    EXPECT_EQ(LifecycleTestScript::updated, 2);
}

TEST(ScriptSystem, ReportsMissingScriptWithContextOnlyOnce) {
    Engine::Diagnostics::instance().clear();
    Engine::Registry registry;
    const auto entity = registry.create();
    registry.add<Engine::ScriptComponent>(entity, Engine::ScriptComponent{"PlayerController"});
    Engine::ScriptSystem system{Engine::ScriptRegistry::instance()};

    system.update(registry, 0.1F);
    system.update(registry, 0.1F);

    const auto diagnostics = Engine::Diagnostics::instance().entries();
    ASSERT_EQ(diagnostics.size(), 1U);
    EXPECT_EQ(diagnostics.front().severity, Engine::DiagnosticSeverity::Warning);
    EXPECT_EQ(diagnostics.front().context.component, "ScriptComponent");
    EXPECT_EQ(diagnostics.front().context.object, "Entity " + std::to_string(entity));
    EXPECT_EQ(diagnostics.front().message,
              "Brak skryptu PlayerController; zarejestruj go lub usuń komponent.");
    EXPECT_FALSE(diagnostics.front().context.suggestedAction.empty());
}

TEST(ScriptSystem, ReportsRuntimeScriptExceptionsInsteadOfPropagatingThem) {
    Engine::Diagnostics::instance().clear();
    auto &scripts = Engine::ScriptRegistry::instance();
    scripts.registerClass<ThrowingTestScript>("ThrowingTestScript", "Tests/ScriptSystemTests.cpp");
    Engine::Scene scene;
    const auto actor = scene.createActor("Broken controller");
    actor.addScript("ThrowingTestScript");

    Engine::ScriptSystem{scripts}.update(scene, 0.1F);

    const auto diagnostics = Engine::Diagnostics::instance().entries();
    ASSERT_EQ(diagnostics.size(), 1U);
    EXPECT_EQ(diagnostics.front().severity, Engine::DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostics.front().context.object, "Broken controller");
    EXPECT_EQ(diagnostics.front().context.component, "ScriptComponent");
    EXPECT_EQ(diagnostics.front().context.file, "Tests/ScriptSystemTests.cpp");
    EXPECT_NE(diagnostics.front().message.find("test failure"), std::string::npos);
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
