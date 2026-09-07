#include "Engine/Scripting/ScriptModuleApi.h"

// Current scripts retain ENGINE_REGISTER_SCRIPT for source compatibility.
// Their static registration runs while this DLL loads, with the generation
// selected by ScriptModuleManager. The explicit entry point keeps the module
// boundary stable and is where generated reflection registration will move.
extern "C" GAME_SCRIPT_API std::uint32_t GE_GetScriptApiVersion() {
    return ENGINE_SCRIPT_API_VERSION;
}

extern "C" GAME_SCRIPT_API void GE_RegisterGameScripts(Engine::ScriptModuleRegistrar *) {}
