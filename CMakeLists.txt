cmake_minimum_required(VERSION 4.4)
project(GamEngine LANGUAGES CXX)

include(cmake/CompilerOptions.cmake)
include(cmake/Dependencies.cmake)

set(GAMEENGINE_GAME_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/DEMO" CACHE PATH "Directory containing GamEngine.project and Assets")
get_filename_component(GAMEENGINE_GAME_ROOT "${GAMEENGINE_GAME_DIRECTORY}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
if(NOT EXISTS "${GAMEENGINE_GAME_ROOT}/GamEngine.project" OR NOT IS_DIRECTORY "${GAMEENGINE_GAME_ROOT}/Assets")
    message(FATAL_ERROR "GAMEENGINE_GAME_DIRECTORY must contain GamEngine.project and Assets: ${GAMEENGINE_GAME_ROOT}")
endif()

option(GE_BUILD_EDITOR "Build GamEngine Editor" ON)
option(GE_BUILD_PLAYER "Build GamEngine Player and game scripts" ON)
option(GE_BUILD_TESTS "Build GamEngine tests" OFF)

# Slang is used to cook built-in shaders and, in a development build, is the
# fallback compiler for Shader Graphs.  Portable Editor packages bundle this
# exact tool instead of relying on PATH or a Vulkan SDK installation.
find_program(GAMEENGINE_SLANGC NAMES slangc REQUIRED)

add_subdirectory(Engine)
include(cmake/Shaders.cmake)
gameengine_add_engine_shaders()

if(GE_BUILD_EDITOR)
    add_subdirectory(Editor)
endif()
if(GE_BUILD_PLAYER)
    add_subdirectory(Player)
endif()
if(GE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Tests)
endif()

if(GE_BUILD_EDITOR)
    include(cmake/Packaging.cmake)
endif()
