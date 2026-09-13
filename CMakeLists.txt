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
add_executable(GenerateBRDFLut Tools/GenerateBRDFLut/main.cpp Engine/Source/Renderer/Lighting/EnvironmentBaker.cpp)
target_include_directories(GenerateBRDFLut PRIVATE "${PROJECT_SOURCE_DIR}/Engine/Include" "${GLM_INCLUDE_DIR}")
gameengine_enable_warnings(GenerateBRDFLut)
add_custom_target(BrdfLutAsset
    # This is project content, not an executable-side cache.  The Editor uses
    # GAMEENGINE_GAME_ROOT directly, while Player copies it beside the binary.
    COMMAND "$<TARGET_FILE:GenerateBRDFLut>" "${GAMEENGINE_GAME_ROOT}/Assets/BRDF/brdf_lut.bin"
    DEPENDS GenerateBRDFLut
    BYPRODUCTS "${GAMEENGINE_GAME_ROOT}/Assets/BRDF/brdf_lut.bin"
    COMMENT "Cooking immutable BRDF LUT asset")
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
