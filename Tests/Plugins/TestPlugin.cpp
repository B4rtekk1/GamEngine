// Fixture plugin for PluginManagerTests. Its behaviour is steered by marker
// files in the plugin folder so one library covers several test cases.
#include <Engine/Plugins/PluginApi.h>
#include <Engine/Plugins/PluginContext.h>
#include <Engine/Scripting/Script.h>
#include <filesystem>
#include <stdexcept>

namespace {
class PluginSpinner final : public Engine::Script {
  public:
    PluginSpinner() {
        Engine::Diagnostics::instance().report(Engine::DiagnosticSeverity::Info, "[TestPlugin] spinner created", {});
    }
    ~PluginSpinner() override {
        Engine::Diagnostics::instance().report(Engine::DiagnosticSeverity::Info, "[TestPlugin] spinner destroyed", {});
    }
    PluginSpinner(const PluginSpinner&) = delete;
    PluginSpinner& operator=(const PluginSpinner&) = delete;

    float speed = 90.0F;
};

class CountingSystem final : public Engine::PluginSystem {
  public:
    explicit CountingSystem(const Engine::PluginContext& context) : name_(context.name()) {}

    void onPlayStart(Engine::Scene&) override {
        log("onPlayStart");
    }
    void onUpdate(Engine::Scene&, float) override {
        log("onUpdate");
    }
    void onFixedUpdate(Engine::Scene&, float) override {
        log("onFixedUpdate");
    }
    void onPlayStop(Engine::Scene&) override {
        log("onPlayStop");
    }

  private:
    void log(const char* callback) const {
        Engine::Diagnostics::instance().report(Engine::DiagnosticSeverity::Info,
                                               "[TestPlugin] " + name_ + " " + callback, {});
    }
    std::string name_;
};
} // namespace

GE_PLUGIN_EXPORT std::uint32_t GE_GetPluginApiVersion() {
    return ENGINE_PLUGIN_API_VERSION;
}

GE_PLUGIN_EXPORT void GE_PluginStartup(Engine::PluginContext* context) {
    if (std::filesystem::exists(context->directory() / "throw_on_startup"))
        throw std::runtime_error("startup refused by test");
    const std::string scriptName = context->name() + "Spinner";
    context->scripts().registerScript<PluginSpinner>(scriptName);
    context->scripts().registerField<PluginSpinner, &PluginSpinner::speed>(scriptName, "Speed");
    context->addSystem<CountingSystem>(*context);
    context->log(Engine::DiagnosticSeverity::Info, "started");
}

GE_PLUGIN_EXPORT void GE_PluginShutdown() {
    Engine::Diagnostics::instance().report(Engine::DiagnosticSeverity::Info, "[TestPlugin] shutdown", {});
}
