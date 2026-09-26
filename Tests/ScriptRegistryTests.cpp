#include <gtest/gtest.h>

#include "Engine/Scripting/Script.h"
#include "Engine/Scripting/ScriptRegistry.h"
#include "Engine/ECS/Components/ScriptComponent.h"

#include <algorithm>

namespace {

class RegistryTestScript final : public Engine::Script {
public:
    int marker{42};
};

class ReplacementRegistryTestScript final : public Engine::Script {
};

TEST(ScriptRegistry, RegistersCreatesAndListsScriptClasses) {
    auto& registry = Engine::ScriptRegistry::instance();
    constexpr auto name = "RegistryTestScript";
    registry.registerClass<RegistryTestScript>(name);

    EXPECT_EQ(registry.className<RegistryTestScript>(), name);
    auto script = registry.create(name);
    auto* typedScript = dynamic_cast<RegistryTestScript*>(script.instance);
    ASSERT_NE(typedScript, nullptr);
    EXPECT_EQ(typedScript->marker, 42);
    EXPECT_FALSE(registry.create("MissingScript"));
    const auto names = registry.classNames();
    EXPECT_NE(std::find(names.begin(), names.end(), name), names.end());
}

TEST(ScriptRegistry, ReplacesFactoryForAnExistingClassName) {
    auto& registry = Engine::ScriptRegistry::instance();
    constexpr auto name = "RegistryTestReplacement";
    registry.registerClass<RegistryTestScript>(name);
    ASSERT_NE(dynamic_cast<RegistryTestScript*>(registry.create(name).instance), nullptr);

    registry.registerClass<ReplacementRegistryTestScript>(name);
    auto replacement = registry.create(name);
    EXPECT_NE(dynamic_cast<ReplacementRegistryTestScript*>(replacement.instance), nullptr);
    EXPECT_EQ(registry.className<ReplacementRegistryTestScript>(), name);
}

TEST(ScriptRegistry, FieldSynchronizationPreservesValuesAndMigratesSchemaChanges) {
    Engine::ScriptRegistry registry;
    Engine::ScriptModuleRegistrar registrar{1};
    registrar.registerScript<RegistryTestScript>("Fields");
    registrar.registerField<RegistryTestScript, &RegistryTestScript::marker>("Fields", "marker");
    auto descriptor = registrar.descriptors().front();
    registry.registerClass(descriptor);
    Engine::ScriptComponent component{"Fields"};
    registry.syncFields(component);
    ASSERT_EQ(std::get<int>(component.fields.at("marker")), 42);
    component.fields.at("marker") = 91;
    registry.syncFields(component);
    EXPECT_EQ(std::get<int>(component.fields.at("marker")), 91);

    // The same field count must not hide a rename during hot reload.
    descriptor.fields.front().name = "renamed";
    descriptor.fields.front().attributes.emplace_back(Engine::ScriptFormerlySerializedAsAttribute{"marker"});
    registry.registerClass(descriptor);
    registry.syncFields(component);
    EXPECT_FALSE(component.fields.contains("marker"));
    EXPECT_EQ(std::get<int>(component.fields.at("renamed")), 91);

    component.fields.at("renamed") = std::string{"wrong type"};
    registry.syncFields(component);
    EXPECT_EQ(std::get<int>(component.fields.at("renamed")), 42);
    component.fields.emplace("obsolete", 7);
    registry.syncFields(component);
    EXPECT_EQ(component.fields.size(), 1u);
    descriptor.fields.clear();
    registry.registerClass(descriptor);
    registry.syncFields(component);
    EXPECT_TRUE(component.fields.empty());
}

} // namespace
