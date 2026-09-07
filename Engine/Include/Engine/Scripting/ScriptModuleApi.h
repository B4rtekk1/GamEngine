#pragma once

#include <cstdint>

// This value protects the small C ABI used between the editor and a gameplay
// module. Increment it whenever ScriptModuleRegistrar's ABI changes.
#define ENGINE_SCRIPT_API_VERSION 2U

#ifdef _WIN32
#  ifdef GAME_SCRIPTS_BUILD_DLL
#    define GAME_SCRIPT_API __declspec(dllexport)
#  else
#    define GAME_SCRIPT_API __declspec(dllimport)
#  endif
#else
#  define GAME_SCRIPT_API
#endif

namespace Engine {
    class ScriptModuleRegistrar;
}

extern "C" {
    GAME_SCRIPT_API std::uint32_t GE_GetScriptApiVersion();
    GAME_SCRIPT_API void GE_RegisterGameScripts(Engine::ScriptModuleRegistrar *registrar);
}
