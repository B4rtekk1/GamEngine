add_executable(Player Source/main.cpp)
# Player is a game-agnostic runtime.  GameScripts.dll is compiled by the
# standalone SDK and loaded through ScriptModuleManager at runtime.
set_target_properties(Player PROPERTIES OUTPUT_NAME GamEnginePlayer)
target_link_libraries(Player PRIVATE Engine)
gameengine_enable_warnings(Player)
gameengine_enable_sanitizers(Player)
gameengine_enable_release_ipo(Player)
gameengine_copy_runtime_dependencies(Player)
