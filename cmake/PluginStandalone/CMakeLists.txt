# Builds a GamEngine plugin against the Editor's SDK. Shipped as
# SDK/Plugin/CMakeLists.txt; configure it once per plugin:
#
#   cmake -S <Editor>/SDK/Plugin -B <Plugin>/Build -DGE_PLUGIN_ROOT=<Plugin> -DGE_SDK_ROOT=<Editor>/SDK
#   cmake --build <Plugin>/Build --config Release
#
# <Plugin> is the plugin folder: its <Name>.geplugin manifest and a Source/
# folder with the plugin's .cpp/.h files. The library lands in
# <Plugin>/Binaries/<Platform>/, where PluginManager looks for it.
cmake_minimum_required(VERSION 3.25)

project(GamEnginePlugin LANGUAGES CXX)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
foreach(required GE_PLUGIN_ROOT GE_SDK_ROOT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()
file(TO_CMAKE_PATH "${GE_PLUGIN_ROOT}" GE_PLUGIN_ROOT)
file(TO_CMAKE_PATH "${GE_SDK_ROOT}" GE_SDK_ROOT)

# The library name comes from the manifest: "library = X", else "name = X".
file(GLOB plugin_manifests "${GE_PLUGIN_ROOT}/*.geplugin")
list(LENGTH plugin_manifests manifest_count)
if(NOT manifest_count EQUAL 1)
    message(FATAL_ERROR "${GE_PLUGIN_ROOT} must contain exactly one .geplugin manifest")
endif()
file(STRINGS "${plugin_manifests}" manifest_lines REGEX "^[ \t]*(name|library)[ \t]*=")
set(plugin_name "")
set(plugin_library "")
foreach(line IN LISTS manifest_lines)
    if(line MATCHES "^[ \t]*name[ \t]*=[ \t]*([^ \t]+)")
        set(plugin_name "${CMAKE_MATCH_1}")
    elseif(line MATCHES "^[ \t]*library[ \t]*=[ \t]*([^ \t]+)")
        set(plugin_library "${CMAKE_MATCH_1}")
    endif()
endforeach()
if(NOT plugin_library)
    set(plugin_library "${plugin_name}")
endif()
if(NOT plugin_library)
    message(FATAL_ERROR "${plugin_manifests} needs a name")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${plugin_manifests}")

file(GLOB_RECURSE plugin_sources CONFIGURE_DEPENDS
    "${GE_PLUGIN_ROOT}/Source/*.cpp" "${GE_PLUGIN_ROOT}/Source/*.cc" "${GE_PLUGIN_ROOT}/Source/*.cxx")
if(NOT plugin_sources)
    message(FATAL_ERROR "${GE_PLUGIN_ROOT}/Source has no C++ sources")
endif()

# Must match PluginManager::platformDirectory().
if(WIN32)
    set(plugin_platform Win64)
    set(engine_link_library "${GE_SDK_ROOT}/Lib/Engine.lib")
elseif(APPLE)
    set(plugin_platform Mac)
    set(engine_link_library "${GE_SDK_ROOT}/Lib/libEngine.dylib")
else()
    set(plugin_platform Linux64)
    set(engine_link_library "${GE_SDK_ROOT}/Lib/libEngine.so")
endif()

add_library(${plugin_library} SHARED ${plugin_sources})
target_compile_features(${plugin_library} PRIVATE cxx_std_23)
target_include_directories(${plugin_library} PRIVATE
    "${GE_PLUGIN_ROOT}/Source"
    "${GE_SDK_ROOT}/Include"
    "${GE_SDK_ROOT}/ThirdParty")
target_link_libraries(${plugin_library} PRIVATE "${engine_link_library}")
# Only the GE_PLUGIN_EXPORT entry points need to be visible.
set_target_properties(${plugin_library} PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN TRUE)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # As for game scripts: GNU-unique symbols would pin the library in memory.
    target_compile_options(${plugin_library} PRIVATE -fno-gnu-unique)
endif()
set(plugin_output "${GE_PLUGIN_ROOT}/Binaries/${plugin_platform}")
set_target_properties(${plugin_library} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${plugin_output}"
    RUNTIME_OUTPUT_DIRECTORY "${plugin_output}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${config}" config_upper)
    set_target_properties(${plugin_library} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_${config_upper} "${plugin_output}"
        RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${plugin_output}")
endforeach()
