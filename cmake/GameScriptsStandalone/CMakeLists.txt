cmake_minimum_required(VERSION 3.25)

project(GamEngineGameScripts LANGUAGES CXX)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

if(NOT DEFINED GE_PROJECT_ROOT)
    message(FATAL_ERROR "GE_PROJECT_ROOT is required")
endif()
if(NOT DEFINED GE_SDK_ROOT)
    message(FATAL_ERROR "GE_SDK_ROOT is required")
endif()

# Normalize native paths before writing CMake glob verification scripts.
# Normalize before using them in CONFIGURE_DEPENDS: CMake later writes these
# globs into VerifyGlobs.cmake, where a literal \U is an invalid escape.
file(TO_CMAKE_PATH "${GE_PROJECT_ROOT}" GE_PROJECT_ROOT)
file(TO_CMAKE_PATH "${GE_SDK_ROOT}" GE_SDK_ROOT)
if(NOT DEFINED GE_ASSET_ROOT)
    set(GE_ASSET_ROOT "${GE_PROJECT_ROOT}/Assets")
endif()
file(TO_CMAKE_PATH "${GE_ASSET_ROOT}" GE_ASSET_ROOT)

file(GLOB_RECURSE GAME_SCRIPT_SOURCES CONFIGURE_DEPENDS
    "${GE_ASSET_ROOT}/Scripts/*.cpp" "${GE_ASSET_ROOT}/Scripts/*.cc" "${GE_ASSET_ROOT}/Scripts/*.cxx")
file(GLOB_RECURSE GAME_SCRIPT_HEADERS CONFIGURE_DEPENDS
    "${GE_ASSET_ROOT}/Scripts/*.h" "${GE_ASSET_ROOT}/Scripts/*.hpp")

set(GENERATED_MODULE "${CMAKE_CURRENT_BINARY_DIR}/GameScriptsModule.generated.cpp")
add_custom_command(
    OUTPUT "${GENERATED_MODULE}"
    COMMAND "${CMAKE_COMMAND}"
        "-DGAME_ROOT:PATH=${GE_PROJECT_ROOT}"
        "-DSCRIPT_ROOT:PATH=${GE_ASSET_ROOT}/Scripts"
        "-DOUTPUT:FILEPATH=${GENERATED_MODULE}"
        -P "${GE_SDK_ROOT}/Tools/GenerateGameScriptsModule.cmake"
    DEPENDS ${GAME_SCRIPT_HEADERS} "${GE_SDK_ROOT}/Tools/GenerateGameScriptsModule.cmake"
    VERBATIM)

add_library(GameScripts SHARED ${GAME_SCRIPT_SOURCES} "${GENERATED_MODULE}")
target_compile_features(GameScripts PRIVATE cxx_std_23)
target_compile_definitions(GameScripts PRIVATE GAME_SCRIPTS_BUILD_DLL)
target_include_directories(GameScripts PRIVATE
    "${GE_ASSET_ROOT}/Scripts"
    "${GE_SDK_ROOT}/Include"
    "${GE_SDK_ROOT}/ThirdParty")
if(WIN32)
    set(ENGINE_LINK_LIBRARY "${GE_SDK_ROOT}/Lib/Engine.lib")
else()
    set(ENGINE_LINK_LIBRARY "${GE_SDK_ROOT}/Lib/libEngine.so")
    set_target_properties(GameScripts PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "$ORIGIN")
endif()
target_link_libraries(GameScripts PRIVATE "${ENGINE_LINK_LIBRARY}")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GNU-unique template/static symbols otherwise pin dlopen modules and can
    # retain state from a previous generation across hot reloads.
    target_compile_options(GameScripts PRIVATE -fno-gnu-unique)
endif()
if(UNIX)
    target_link_options(GameScripts PRIVATE "-Wl,--no-undefined")
endif()
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
set_target_properties(GameScripts PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${GAME_SCRIPTS_OUTPUT_DIRECTORY}"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG "${GAME_SCRIPTS_OUTPUT_DIRECTORY}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${GAME_SCRIPTS_OUTPUT_DIRECTORY}"
    LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${GAME_SCRIPTS_OUTPUT_DIRECTORY}"
    LIBRARY_OUTPUT_DIRECTORY_MINSIZEREL "${GAME_SCRIPTS_OUTPUT_DIRECTORY}")
