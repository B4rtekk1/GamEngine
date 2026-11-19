// Sample GamEngine plugin. Build it with the SDK's Plugin template:
//
//   cmake -S <Editor>/SDK/Plugin -B Build -DGE_PLUGIN_ROOT=<this folder> -DGE_SDK_ROOT=<Editor>/SDK
//   cmake --build Build --config Release
//
// then copy (or keep) this folder in <Project>/Plugins/ and open the project.
#include <Engine/Plugins/PluginApi.h>
#include <Engine/Plugins/PluginContext.h>
#include <Engine/Scripting/Script.h>
#include <cstdint>
#include <string>

namespace {
// Spins its object around Y. Speed shows up in the Inspector.
class HelloSpinner final : public Engine::Script {
  public:
    float speed = 45.0F;

    void onUpdate(const float deltaTime) override {
        rotate({0.0F, speed * deltaTime, 0.0F});
    }
};

// Counts the frames of each play session.
class FrameCounter final : public Engine::PluginSystem {
  public:
    explicit FrameCounter(const Engine::PluginContext& context) : context_(context) {}

    void onPlayStart(Engine::Scene&) override {
        frames_ = 0;
        context_.log(Engine::DiagnosticSeverity::Info, "Play started");
    }
    void onUpdate(Engine::Scene&, float) override {
        ++frames_;
    }
    void onPlayStop(Engine::Scene&) override {
        context_.log(Engine::DiagnosticSeverity::Info, "Play stopped after " + std::to_string(frames_) + " frames");
    }

  private:
    const Engine::PluginContext& context_;
    std::uint64_t frames_ = 0;
};
} // namespace

GE_PLUGIN_EXPORT std::uint32_t GE_GetPluginApiVersion() {
    return ENGINE_PLUGIN_API_VERSION;
}

GE_PLUGIN_EXPORT void GE_PluginStartup(Engine::PluginContext* context) {
    context->scripts().registerScript<HelloSpinner>("HelloSpinner");
    context->scripts().registerField<HelloSpinner, &HelloSpinner::speed>("HelloSpinner", "Speed");
    context->addSystem<FrameCounter>(*context);
    context->log(Engine::DiagnosticSeverity::Info, "Hello from a plugin");
}
