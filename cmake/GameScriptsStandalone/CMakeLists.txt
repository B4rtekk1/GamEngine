cmake_minimum_required(VERSION 3.25)

project(GamEngineGameScripts LANGUAGES CXX)

if(NOT DEFINED GE_PROJECT_ROOT)
    message(FATAL_ERROR "GE_PROJECT_ROOT is required")
endif()
if(NOT DEFINED GE_SDK_ROOT)
    message(FATAL_ERROR "GE_SDK_ROOT is required")
endif()

# Values originate in Editor.exe and therefore use native Windows separators.
# Normalize before using them in CONFIGURE_DEPENDS: CMake later writes these
# globs into VerifyGlobs.cmake, where a literal \U is an invalid escape.
file(TO_CMAKE_PATH "${GE_PROJECT_ROOT}" GE_PROJECT_ROOT)
file(TO_CMAKE_PATH "${GE_SDK_ROOT}" GE_SDK_ROOT)

file(GLOB_RECURSE GAME_SCRIPT_SOURCES CONFIGURE_DEPENDS
    "${GE_PROJECT_ROOT}/Assets/Scripts/*.cpp")
file(GLOB_RECURSE GAME_SCRIPT_HEADERS CONFIGURE_DEPENDS
    "${GE_PROJECT_ROOT}/Assets/Scripts/*.h")

set(GENERATED_MODULE "${CMAKE_CURRENT_BINARY_DIR}/GameScriptsModule.generated.cpp")
add_custom_command(
    OUTPUT "${GENERATED_MODULE}"
    COMMAND "${CMAKE_COMMAND}"
        "-DGAME_ROOT:PATH=${GE_PROJECT_ROOT}"
        "-DOUTPUT:FILEPATH=${GENERATED_MODULE}"
        -P "${GE_SDK_ROOT}/Tools/GenerateGameScriptsModule.cmake"
    DEPENDS ${GAME_SCRIPT_HEADERS} "${GE_SDK_ROOT}/Tools/GenerateGameScriptsModule.cmake"
    VERBATIM)

add_library(GameScripts SHARED ${GAME_SCRIPT_SOURCES} "${GENERATED_MODULE}")
target_compile_features(GameScripts PRIVATE cxx_std_23)
target_compile_definitions(GameScripts PRIVATE GAME_SCRIPTS_BUILD_DLL)
target_include_directories(GameScripts PRIVATE
    "${GE_PROJECT_ROOT}/Assets/Scripts"
    "${GE_SDK_ROOT}/Include"
    "${GE_SDK_ROOT}/ThirdParty")
target_link_libraries(GameScripts PRIVATE "${GE_SDK_ROOT}/Lib/Engine.lib")
target_precompile_headers(GameScripts PRIVATE "${GE_SDK_ROOT}/GameScriptsPCH.h")

# Do not let multi-config generators append Debug/Release.  The Editor always
# loads this stable project location and ScriptModuleManager versions copies.
set(GAME_SCRIPTS_OUTPUT_DIRECTORY "${GE_PROJECT_ROOT}/Library/ScriptModules")
set_target_properties(GameScripts PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${GAME_SCRIPTS_OUTPUT_DIRECTORY}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${GAME_SCRIPTS_OUTPUT_DIRECTORY}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${GAME_SCRIPTS_OUTPUT_DIRECTORY}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${GAME_SCRIPTS_OUTPUT_DIRECTORY}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${GAME_SCRIPTS_OUTPUT_DIRECTORY}")
