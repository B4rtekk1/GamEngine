file(GLOB_RECURSE GAME_SCRIPT_SOURCES CONFIGURE_DEPENDS "${GAMEENGINE_GAME_ROOT}/Assets/Scripts/*.cpp")
file(GLOB_RECURSE GAME_SCRIPT_HEADERS CONFIGURE_DEPENDS "${GAMEENGINE_GAME_ROOT}/Assets/Scripts/*.h")
set(GENERATED_GAME_SCRIPTS_MODULE "${CMAKE_CURRENT_BINARY_DIR}/GameScriptsModule.generated.cpp")
add_custom_command(OUTPUT "${GENERATED_GAME_SCRIPTS_MODULE}"
    COMMAND ${CMAKE_COMMAND} -DGAME_ROOT:PATH=${GAMEENGINE_GAME_ROOT} -DOUTPUT:FILEPATH=${GENERATED_GAME_SCRIPTS_MODULE} -P "${PROJECT_SOURCE_DIR}/Tools/GenerateGameScriptsModule.cmake"
    DEPENDS ${GAME_SCRIPT_HEADERS} "${PROJECT_SOURCE_DIR}/Tools/GenerateGameScriptsModule.cmake" VERBATIM)
add_library(GameScripts SHARED ${GAME_SCRIPT_SOURCES} "${GENERATED_GAME_SCRIPTS_MODULE}")
target_link_libraries(GameScripts PRIVATE Engine)
target_compile_definitions(GameScripts PRIVATE GAME_SCRIPTS_BUILD_DLL)
target_include_directories(GameScripts PRIVATE "${GAMEENGINE_GAME_ROOT}/Assets/Scripts")
target_precompile_headers(GameScripts PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/GameScriptsPCH.h")
gameengine_enable_warnings(GameScripts)
gameengine_enable_sanitizers(GameScripts)
add_executable(Player Source/main.cpp)
set_target_properties(Player PROPERTIES OUTPUT_NAME GamEngine)
target_link_libraries(Player PRIVATE Engine GameScripts)
gameengine_enable_warnings(Player)
gameengine_enable_sanitizers(Player)
gameengine_copy_runtime_dependencies(Player)
if(TARGET Editor)
    add_custom_target(GameScriptsForEditor
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:GameScripts> $<TARGET_FILE_DIR:Editor>
        DEPENDS GameScripts COMMENT "Copying game scripts for explicit Editor hot-reload testing")
endif()
add_custom_target(GameProjectAssets
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Player>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${GAMEENGINE_SHADER_OUTPUT_DIR}" "$<TARGET_FILE_DIR:Player>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${GAMEENGINE_GAME_ROOT}/Assets" "$<TARGET_FILE_DIR:Player>/Assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GAMEENGINE_GAME_ROOT}/GamEngine.project" "$<TARGET_FILE_DIR:Player>/GamEngine.project"
    DEPENDS EngineShaders Player COMMENT "Copying assets for Player")
add_custom_target(Demo DEPENDS Player GameProjectAssets)
