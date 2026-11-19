#pragma once

#include <cstdint>

/**
 * Entry points of a GamEngine plugin library.
 *
 * A plugin is a folder with a manifest (<Name>.geplugin) and a shared
 * library that exports these C functions. PluginManager loads it, checks
 * GE_GetPluginApiVersion() against ENGINE_PLUGIN_API_VERSION and calls
 * GE_PluginStartup() once with a PluginContext through which the plugin
 * registers its scripts and systems. GE_PluginShutdown() is optional and runs
 * after everything the plugin registered has been released.
 *
 * Plugins link the Engine library and use its C++ API, so they must be built
 * with the same compiler and runtime as the Engine (the SDK's Plugin
 * template does this), exactly like the game's script module.
 *
 *     #include <Engine/Plugins/PluginApi.h>
 *     #include <Engine/Plugins/PluginContext.h>
 *
 *     GE_PLUGIN_EXPORT std::uint32_t GE_GetPluginApiVersion() { return ENGINE_PLUGIN_API_VERSION; }
 *     GE_PLUGIN_EXPORT void GE_PluginStartup(Engine::PluginContext* context) {
 *         context->scripts().registerScript<Spinner>("Spinner");
 *         context->addSystem<WindSystem>();
 *     }
 */

// Increment whenever PluginContext, PluginSystem or these entry points change
// in a way that breaks already compiled plugins.
#define ENGINE_PLUGIN_API_VERSION 1U

#ifdef _WIN32
#define GE_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define GE_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace Engine {
class PluginContext;
}

// Signatures PluginManager resolves; plugins define them with GE_PLUGIN_EXPORT.
using GE_GetPluginApiVersionFunction = std::uint32_t (*)();
using GE_PluginStartupFunction = void (*)(Engine::PluginContext* context);
using GE_PluginShutdownFunction = void (*)();
