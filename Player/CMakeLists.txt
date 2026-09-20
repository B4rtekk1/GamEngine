add_executable(Player Source/main.cpp)
# UserPaths is part of the runtime bootstrap as well as the editor.
if(WIN32)
    target_sources(Player PRIVATE ${PROJECT_SOURCE_DIR}/Platform/Windows/WindowsUserPaths.cpp)
elseif(UNIX)
    target_sources(Player PRIVATE ${PROJECT_SOURCE_DIR}/Platform/Linux/LinuxUserPaths.cpp)
endif()
# Player is a game-agnostic runtime.  GameScripts.dll is compiled by the
# standalone SDK and loaded through ScriptModuleManager at runtime.
set_target_properties(Player PROPERTIES OUTPUT_NAME GamEnginePlayer)
target_include_directories(Player PRIVATE "${PROJECT_SOURCE_DIR}")
target_link_libraries(Player PRIVATE Engine)
gameengine_enable_warnings(Player)
gameengine_enable_sanitizers(Player)
gameengine_enable_release_ipo(Player)
gameengine_copy_runtime_dependencies(Player)
