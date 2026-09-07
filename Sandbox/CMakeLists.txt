# Playable scene module. Add gameplay C++ scripts under Assets/Scripts/.
# CMake reconfigures automatically when a generated script source is added.
file(GLOB_RECURSE GAME_SCRIPT_SOURCES CONFIGURE_DEPENDS
        "${GAMEENGINE_GAME_ROOT}/Assets/Scripts/*.cpp")

if(NOT EXISTS "${GAMEENGINE_GAME_ROOT}/GamEngine.project")
    message(FATAL_ERROR
            "GAMEENGINE_GAME_DIRECTORY must contain GamEngine.project: ${GAMEENGINE_GAME_ROOT}")
endif()
if(NOT IS_DIRECTORY "${GAMEENGINE_GAME_ROOT}/Assets")
    message(FATAL_ERROR
            "GAMEENGINE_GAME_DIRECTORY must contain an Assets directory: ${GAMEENGINE_GAME_ROOT}")
endif()

add_library(GameScripts SHARED
        ${GAME_SCRIPT_SOURCES}
        Source/GameScriptsModule.cpp)
gameengine_enable_warnings(GameScripts)
gameengine_enable_sanitizers(GameScripts)
target_link_libraries(GameScripts PRIVATE Engine)
target_compile_definitions(GameScripts PRIVATE GAME_SCRIPTS_BUILD_DLL)

add_executable(GamEngine
        Source/main.cpp)
gameengine_enable_warnings(GamEngine)
gameengine_enable_sanitizers(GamEngine)
target_link_libraries(GamEngine PRIVATE Engine GameScripts)
gameengine_copy_runtime_dependencies(GamEngine)
target_compile_definitions(GamEngine PRIVATE GAMEENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_dependencies(GamEngine Shaders)

# Editor discovers gameplay types solely through this versioned dynamic module.
add_dependencies(Editor GameScripts)
add_custom_target(GameScriptsForEditor ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:GameScripts> $<TARGET_FILE_DIR:Editor>
        DEPENDS GameScripts)
add_dependencies(Editor GameScriptsForEditor)

add_custom_target(GamEngineAssets ALL
        DEPENDS ${SHADER_OUTPUTS}
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:GamEngine>/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${SHADER_OUT_DIR}" "$<TARGET_FILE_DIR:GamEngine>/shaders"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Editor>/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${SHADER_OUT_DIR}" "$<TARGET_FILE_DIR:Editor>/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${GAMEENGINE_GAME_ROOT}/Assets" "$<TARGET_FILE_DIR:GamEngine>/Assets"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${GAMEENGINE_GAME_ROOT}/Assets" "$<TARGET_FILE_DIR:Editor>/Assets"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${GAMEENGINE_GAME_ROOT}/GamEngine.project" "$<TARGET_FILE_DIR:GamEngine>/GamEngine.project"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${GAMEENGINE_GAME_ROOT}/GamEngine.project" "$<TARGET_FILE_DIR:Editor>/GamEngine.project"
        COMMENT "Copying scene assets and shaders beside GamEngine")
add_dependencies(GamEngineAssets Shaders)
add_dependencies(GamEngine GamEngineAssets)
