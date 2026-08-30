#include <gtest/gtest.h>

#include "Engine/Scripting/Script.h"
#include "Engine/Scripting/ScriptRegistry.h"

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
    auto* typedScript = dynamic_cast<RegistryTestScript*>(script.get());
    ASSERT_NE(typedScript, nullptr);
    EXPECT_EQ(typedScript->marker, 42);
    EXPECT_EQ(registry.create("MissingScript"), nullptr);
    const auto names = registry.classNames();
    EXPECT_NE(std::find(names.begin(), names.end(), name), names.end());
}

TEST(ScriptRegistry, ReplacesFactoryForAnExistingClassName) {
    auto& registry = Engine::ScriptRegistry::instance();
    constexpr auto name = "RegistryTestReplacement";
    registry.registerClass<RegistryTestScript>(name);
    ASSERT_NE(dynamic_cast<RegistryTestScript*>(registry.create(name).get()), nullptr);

    registry.registerClass<ReplacementRegistryTestScript>(name);
    auto replacement = registry.create(name);
    EXPECT_NE(dynamic_cast<ReplacementRegistryTestScript*>(replacement.get()), nullptr);
    EXPECT_EQ(registry.className<ReplacementRegistryTestScript>(), name);
}

} // namespace
