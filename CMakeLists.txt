cmake_minimum_required(VERSION 4.4)
project(GamEngine LANGUAGES CXX)

include(cmake/CompilerOptions.cmake)
include(cmake/Dependencies.cmake)

# FidelityFX owns its Vulkan shader permutations independently of GamEngine's Slang shaders.
set(FFX_API_BACKEND VK_X64)
set(FFX_ALL OFF)
set(FFX_BRIXELIZER ON)
set(FFX_BRIXELIZER_GI OFF)
set(FFX_BUILD_AS_DLL OFF)
set(FFX_AUTO_COMPILE_SHADERS ON)
set(_ge_linker_flags_before_ffx "${CMAKE_EXE_LINKER_FLAGS}")
if(WIN32 AND CMAKE_GENERATOR_PLATFORM STREQUAL "")
    # SDK 1.1.4 determines its library suffix from this exact flag.
    set(CMAKE_EXE_LINKER_FLAGS "/machine:x64")
endif()
add_subdirectory(ThridParty/FidelityFX-SDK/sdk)
set(CMAKE_EXE_LINKER_FLAGS "${_ge_linker_flags_before_ffx}")
# The 1.1.4 Vulkan backend includes Frame Interpolation swapchain sources even
# when that effect is disabled. They are independent of Brixelizer and fail to
# compile with the project's newer MSVC toolset.
get_target_property(_ge_ffx_vk_sources ffx_backend_vk_x64 SOURCES)
list(FILTER _ge_ffx_vk_sources EXCLUDE REGEX "[/\\\\]FrameInterpolationSwapchain[/\\\\]")
set_property(TARGET ffx_backend_vk_x64 PROPERTY SOURCES "${_ge_ffx_vk_sources}")

set(GAMEENGINE_GAME_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/MyGame" CACHE PATH "Directory containing GamEngine.project and Assets")
get_filename_component(GAMEENGINE_GAME_ROOT "${GAMEENGINE_GAME_DIRECTORY}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
if(NOT EXISTS "${GAMEENGINE_GAME_ROOT}/GamEngine.project" OR NOT IS_DIRECTORY "${GAMEENGINE_GAME_ROOT}/Assets")
    message(FATAL_ERROR "GAMEENGINE_GAME_DIRECTORY must contain GamEngine.project and Assets: ${GAMEENGINE_GAME_ROOT}")
endif()

option(GE_BUILD_EDITOR "Build GamEngine Editor" ON)
option(GE_BUILD_PLAYER "Build GamEngine Player and game scripts" ON)
option(GE_BUILD_TESTS "Build GamEngine tests" OFF)
option(GE_BUILD_BENCHMARKS "Build CPU scene-update microbenchmarks" OFF)
if(GE_BUILD_BENCHMARKS)
    add_executable(BenchmarkSceneUpdates
        Tools/BenchmarkSceneUpdates/main.cpp
        Engine/Source/Scene/TransformSystem.cpp)
    target_include_directories(BenchmarkSceneUpdates PRIVATE Engine/Include ${GLM_INCLUDE_DIR})
    gameengine_enable_warnings(BenchmarkSceneUpdates)
    add_executable(BenchmarkScriptFields
        Tools/BenchmarkScriptFields/main.cpp
        Engine/Source/Scripting/ScriptRegistry.cpp)
    target_include_directories(BenchmarkScriptFields PRIVATE Engine/Include ${GLM_INCLUDE_DIR})
    gameengine_enable_warnings(BenchmarkScriptFields)
endif()

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
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GAMEENGINE_GAME_ROOT}/Assets/BRDF"
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

if(GE_BUILD_EDITOR OR GE_BUILD_PLAYER)
    include(cmake/Packaging.cmake)
endif()
