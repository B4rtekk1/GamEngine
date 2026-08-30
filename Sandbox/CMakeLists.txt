# Playable scene module. Add gameplay C++ scripts under Source/Scripts/.
# CMake reconfigures automatically when a generated script source is added.
file(GLOB_RECURSE GAME_SCRIPT_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/Scripts/*.cpp")

add_executable(GamEngine
        Source/main.cpp
        ${GAME_SCRIPT_SOURCES})
gameengine_enable_warnings(GamEngine)
gameengine_enable_sanitizers(GamEngine)
target_link_libraries(GamEngine PRIVATE Engine)
gameengine_copy_runtime_dependencies(GamEngine)
target_compile_definitions(GamEngine PRIVATE GAMEENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_dependencies(GamEngine Shaders)

# Game View executes the same native behaviours as the standalone game.  The
# editor therefore needs the gameplay translation units too, so their static
# registrations populate ScriptRegistry in the editor process.
target_sources(Editor PRIVATE ${GAME_SCRIPT_SOURCES})

# A project descriptor is optional. Games without one use the engine's
# defaults, so its absence must not make the asset-copy target fail.
set(GAMEENGINE_PROJECT_COPY_COMMANDS)
if (EXISTS "${CMAKE_SOURCE_DIR}/GamEngine.project")
    list(APPEND GAMEENGINE_PROJECT_COPY_COMMANDS
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_SOURCE_DIR}/GamEngine.project" "$<TARGET_FILE_DIR:GamEngine>/GamEngine.project"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_SOURCE_DIR}/GamEngine.project" "$<TARGET_FILE_DIR:Editor>/GamEngine.project")
endif ()

add_custom_target(GamEngineAssets ALL
        DEPENDS ${SHADER_OUTPUTS}
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:GamEngine>/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${SHADER_OUT_DIR}" "$<TARGET_FILE_DIR:GamEngine>/shaders"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Editor>/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${SHADER_OUT_DIR}" "$<TARGET_FILE_DIR:Editor>/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/Assets" "$<TARGET_FILE_DIR:GamEngine>/Assets"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/Assets" "$<TARGET_FILE_DIR:Editor>/Assets"
        ${GAMEENGINE_PROJECT_COPY_COMMANDS}
        COMMENT "Copying scene assets and shaders beside GamEngine")
add_dependencies(GamEngineAssets Shaders)
add_dependencies(GamEngine GamEngineAssets)
