#include "Engine/Core/Diagnostics.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scripting/Console.h"
#include "Engine/Scripting/Script.h"
#include "Engine/Scripting/ScriptRegistry.h"
#include "Engine/Scripting/ScriptSystem.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

std::vector<Engine::Diagnostic> logged() {
    return Engine::Diagnostics::instance().entries();
}

class ConsoleTestScript final : public Engine::Script {
  public:
    void onUpdate(const float deltaTime) override {
        console.group("Update");
        console.log("dt:", deltaTime);
        // Left open on purpose: it must not indent the next callback.
    }
};

class Streamed final {};
std::ostream& operator<<(std::ostream& stream, const Streamed&) {
    return stream << "streamed";
}

} // namespace

TEST(Console, FormatsWithReplacementFieldsAndJoinsOtherwise) {
    Engine::Diagnostics::instance().clear();
    Engine::console.log("Health: {}", 42);
    Engine::console.log("Health", 42, Engine::Vec3{1.0F, 2.0F, 3.0F});
    Engine::console.log("Literal {}");
    Engine::console.log(std::string{"{runtime}"}, true);
    Engine::console.log(Streamed{});
    Engine::console.log(7);

    const auto entries = logged();
    ASSERT_EQ(entries.size(), 6U);
    EXPECT_EQ(entries[0].message, "Health: 42");
    EXPECT_EQ(entries[1].message, "Health 42 (1, 2, 3)");
    EXPECT_EQ(entries[2].message, "Literal {}");
    EXPECT_EQ(entries[3].message, "{runtime} true");
    EXPECT_EQ(entries[4].message, "streamed");
    EXPECT_EQ(entries[5].message, "7");
    EXPECT_EQ(entries[0].context.subsystem, "Script");
}

TEST(Console, MapsLevelsToSeverities) {
    Engine::Diagnostics::instance().clear();
    Engine::console.info("i");
    Engine::console.debug("d {}", 1);
    Engine::console.warn("w");
    Engine::console.error("e", 2);
    Engine::console.assertThat(true, "never");
    Engine::console.assertThat(false, "Bad value {}", 3);

    const auto entries = logged();
    ASSERT_EQ(entries.size(), 5U);
    EXPECT_EQ(entries[0].severity, Engine::DiagnosticSeverity::Info);
    EXPECT_EQ(entries[1].message, "[debug] d 1");
    EXPECT_EQ(entries[2].severity, Engine::DiagnosticSeverity::Warning);
    EXPECT_EQ(entries[3].severity, Engine::DiagnosticSeverity::Error);
    EXPECT_EQ(entries[3].message, "e 2");
    EXPECT_EQ(entries[4].severity, Engine::DiagnosticSeverity::Error);
    EXPECT_EQ(entries[4].message, "Assertion failed: Bad value 3");
}

TEST(Console, CountsTimesAndGroups) {
    Engine::Diagnostics::instance().clear();
    Engine::console.count("hit");
    Engine::console.count("hit");
    Engine::console.countReset("hit");
    Engine::console.count("hit");
    Engine::console.time("load");
    Engine::console.time("load");
    Engine::console.timeEnd("load");
    Engine::console.timeEnd("load");
    Engine::console.group("Outer");
    Engine::console.log("inner");
    Engine::console.groupEnd();
    Engine::console.groupEnd();
    Engine::console.log("outer");

    const auto entries = logged();
    ASSERT_EQ(entries.size(), 9U);
    EXPECT_EQ(entries[0].message, "hit: 1");
    EXPECT_EQ(entries[1].message, "hit: 2");
    EXPECT_EQ(entries[2].message, "hit: 1");
    EXPECT_EQ(entries[3].severity, Engine::DiagnosticSeverity::Warning);
    EXPECT_EQ(entries[3].message, "Timer 'load' already exists");
    EXPECT_TRUE(entries[4].message.starts_with("load: "));
    EXPECT_TRUE(entries[4].message.ends_with(" ms"));
    EXPECT_EQ(entries[5].message, "Timer 'load' does not exist");
    EXPECT_EQ(entries[6].message, "Outer");
    EXPECT_EQ(entries[7].message, "  inner");
    EXPECT_EQ(entries[8].message, "outer");
}

TEST(Console, NamesTheScriptThatLogged) {
    Engine::Diagnostics::instance().clear();
    auto& scripts = Engine::ScriptRegistry::instance();
    scripts.registerClass<ConsoleTestScript>("ConsoleTestScript", "Tests/ConsoleTests.cpp");
    Engine::Scene scene;
    scene.createActor("Logger").addScript("ConsoleTestScript");
    Engine::ScriptSystem system{scripts};

    system.update(scene, 0.5F);
    system.update(scene, 0.25F);
    Engine::console.log("after");

    const auto entries = logged();
    ASSERT_EQ(entries.size(), 5U);
    EXPECT_EQ(entries[0].message, "Update");
    EXPECT_EQ(entries[1].message, "  dt: 0.5");
    // The group left open by the first update ended with that callback.
    EXPECT_EQ(entries[2].message, "Update");
    EXPECT_EQ(entries[3].message, "  dt: 0.25");
    EXPECT_EQ(entries[1].context.subsystem, "Script");
    EXPECT_EQ(entries[1].context.object, "Logger");
    EXPECT_EQ(entries[1].context.component, "ConsoleTestScript");
    EXPECT_EQ(entries[1].context.file, "Tests/ConsoleTests.cpp");
    EXPECT_EQ(entries[4].message, "after");
    EXPECT_TRUE(entries[4].context.object.empty());
}
