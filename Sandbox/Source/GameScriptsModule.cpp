#include "Engine/Scripting/ScriptModuleApi.h"

#include "PlayerController.h"

extern "C" GAME_SCRIPT_API std::uint32_t GE_GetScriptApiVersion() {
    return ENGINE_SCRIPT_API_VERSION;
}

extern "C" GAME_SCRIPT_API void GE_RegisterGameScripts(Engine::ScriptModuleRegistrar *registrar) {
    registrar->registerScript<PlayerController>("PlayerController", "Assets/Scripts/PlayerController.cpp");
}
