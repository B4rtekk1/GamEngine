cmake_minimum_required(VERSION 4.4)

project(GamEngine)

include(CTest)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(GAMEENGINE_ENABLE_WARNINGS "Enable strict compiler warnings" ON)
option(GAMEENGINE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(GAMEENGINE_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer where supported" OFF)

function(gameengine_enable_warnings target)
    if(NOT GAMEENGINE_ENABLE_WARNINGS)
        return()
    endif()

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if(GAMEENGINE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(GAMEENGINE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

function(gameengine_enable_sanitizers target)
    if(NOT GAMEENGINE_ENABLE_SANITIZERS)
        return()
    endif()

    get_target_property(target_type ${target} TYPE)

    if(MSVC)
        target_compile_options(${target} PRIVATE /fsanitize=address)
        if(NOT target_type STREQUAL "STATIC_LIBRARY")
            target_link_options(${target} PRIVATE /INFERASANLIBS)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer)
        if(NOT target_type STREQUAL "STATIC_LIBRARY")
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        endif()
    endif()
endfunction()

function(gameengine_copy_runtime_dependencies target)
    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_RUNTIME_DLLS:${target}> $<TARGET_FILE_DIR:${target}>
            COMMAND_EXPAND_LISTS)
    endif()
endfunction()

find_package(Vulkan 1.4 REQUIRED)
if(DEFINED ENV{VULKAN_SDK} AND NOT "$ENV{VULKAN_SDK}" STREQUAL "")
    set(SDL3_DIR "$ENV{VULKAN_SDK}/cmake")
endif()
find_package(SDL3 CONFIG REQUIRED)

include(FetchContent)

# Official NVIDIA PhysX 5.10.0.  It is added as a CMake subproject rather
# than imported from vcpkg, so the engine always uses this exact SDK version.
# CPU-only/static avoids a CUDA dependency and removes PhysX runtime DLLs.
set(PX_GENERATE_GPU_PROJECTS OFF CACHE BOOL "Build PhysX GPU projects" FORCE)
set(PX_GENERATE_STATIC_LIBRARIES ON CACHE BOOL "Build PhysX static libraries" FORCE)
set(PX_BUILDSNIPPETS OFF CACHE BOOL "Build PhysX snippets" FORCE)
set(PX_BUILDPVDRUNTIME OFF CACHE BOOL "Build PhysX PVD runtime" FORCE)
# PhysX defaults this internal output label to x86 on Windows when no XML
# preset is used.  Keep the subproject's CPU-only static setup and correctly
# label a 64-bit Visual Studio build without letting a shared-library preset
# override those options.
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(PX_OUTPUT_ARCH "x64" CACHE INTERNAL "PhysX output architecture" FORCE)
endif()
FetchContent_Declare(
        NVIDIA_PhysX
        GIT_REPOSITORY https://github.com/NVIDIA-Omniverse/PhysX.git
        GIT_TAG 3ca45ad36e9755f7c8c5bea9f7c57d308d9f0c54 # ovphysx-0.5.10
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR physx)
FetchContent_MakeAvailable(NVIDIA_PhysX)

# PhysX 5.10 enables /WX for its own sources.  MSVC 19.44 introduced the
# C5054/C5055 enum-migration warnings for existing PhysX expressions.  Use
# Microsoft's compatibility warning wave for third-party targets only;
# GameEngine targets retain their normal warning policy.
if(MSVC)
    function(gameengine_suppress_physx_c5054 directory)
        get_property(physx_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
        foreach(physx_target IN LISTS physx_targets)
            get_target_property(physx_target_type ${physx_target} TYPE)
            if(NOT physx_target_type STREQUAL "INTERFACE_LIBRARY")
                target_compile_options(${physx_target} PRIVATE /Wv:18)
            endif()
        endforeach()

        get_property(physx_subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
        foreach(physx_subdirectory IN LISTS physx_subdirectories)
            gameengine_suppress_physx_c5054("${physx_subdirectory}")
        endforeach()
    endfunction()
    gameengine_suppress_physx_c5054("${nvidia_physx_SOURCE_DIR}/physx")
endif()

FetchContent_Declare(
        DearImGui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG docking
        GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(DearImGui)

add_library(DearImGui STATIC
        ${dearimgui_SOURCE_DIR}/imgui.cpp
        ${dearimgui_SOURCE_DIR}/imgui_demo.cpp
        ${dearimgui_SOURCE_DIR}/imgui_draw.cpp
        ${dearimgui_SOURCE_DIR}/imgui_tables.cpp
        ${dearimgui_SOURCE_DIR}/imgui_widgets.cpp
        ${dearimgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${dearimgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
target_include_directories(DearImGui PUBLIC
        ${dearimgui_SOURCE_DIR}
        ${dearimgui_SOURCE_DIR}/backends)
target_link_libraries(DearImGui PUBLIC SDL3::SDL3 Vulkan::Vulkan)
find_path(GLM_INCLUDE_DIR NAMES glm/glm.hpp HINTS "$ENV{VULKAN_SDK}/Include" REQUIRED)
find_path(VMA_INCLUDE_DIR NAMES vk_mem_alloc.h HINTS "$ENV{VULKAN_SDK}/Include/vma" REQUIRED)

add_library(Engine
        Engine/Source/Application.cpp
        Engine/Source/Assets/StbImage.cpp
        Engine/Source/Assets/AssetManager.cpp
        Engine/Source/Assets/GlbLoader.cpp
        Engine/Source/Assets/Content.cpp
        Engine/Source/Core/Camera.cpp
        Engine/Source/Core/Time.cpp
        Engine/Source/ECS/Registry.cpp
        Engine/Source/Input/Input.cpp
        Engine/Source/Scene/SceneSerializer.cpp
        Engine/Source/Scene/Scene.cpp
        Engine/Source/Scene/Actor.cpp
        Engine/Source/Scene/Prefab.cpp
        Engine/Source/Scene/ScenePresets.cpp
        Engine/Source/Physics/PhysXPhysicsSystem.cpp
        Engine/Source/Scripting/ScriptSystem.cpp
        Engine/Source/Scripting/Script.cpp
        Engine/Source/Scripting/ScriptRegistry.cpp
        Engine/Source/UI/Canvas.cpp
        Engine/Source/UI/CanvasRenderer.cpp
        Engine/Source/UI/UIBatch.cpp
        Engine/Source/UI/Vulkan/UIFontAtlas.cpp
        Engine/Source/UI/Vulkan/UIFontAtlasRuntime.cpp
        Engine/Source/UI/RectTransform.cpp
        Engine/Source/UI/Vulkan/UIPipeline.cpp
        Engine/Source/Renderer/shader_loader.cpp
        Engine/Source/Renderer/Textures/Cubemap.cpp
        Engine/Source/Renderer/Textures/Texture2D.cpp
        Engine/Source/Renderer/Skybox/Skybox.cpp
        Engine/Source/Renderer/Skybox/SkyboxPipeline.cpp
        Engine/Source/Renderer/Lighting/LightingBuffer.cpp
        Engine/Source/Renderer/Passes/ForwardPass.cpp
        Engine/Source/Renderer/Passes/ShadowPass.cpp
        Engine/Source/Renderer/Passes/SkyPass.cpp
        Engine/Source/Renderer/Passes/TonemapPass.cpp
        Engine/Source/Renderer/Culling/GPUCullingPass.cpp
        Engine/Source/Renderer/Culling/IndexedIndirectDrawCount.cpp
        Engine/Source/Renderer/Culling/HiZBuffer.cpp
        Engine/Source/Renderer/Culling/HiZPass.cpp
        Engine/Source/Renderer/Vulkan/buffer.cpp
        Engine/Source/Renderer/Vulkan/depth_buffer.cpp
        Engine/Source/Renderer/Vulkan/graphics_pipeline.cpp
        Engine/Source/Renderer/Vulkan/hdr_buffer.cpp
        Engine/Source/Renderer/Vulkan/msaa.cpp
        Engine/Source/Renderer/Vulkan/renderer.cpp
        Engine/Source/Renderer/Vulkan/renderer_scene_helpers.cpp
        Engine/Source/Renderer/Vulkan/CameraController.cpp
        Engine/Source/Renderer/Vulkan/ViewportRenderTarget.cpp
        Engine/Source/Renderer/Vulkan/shadow_map.cpp
        Engine/Source/Renderer/Vulkan/swapchain.cpp
        Engine/Source/Renderer/Vulkan/vulkan_device.cpp
        Engine/Source/Renderer/Vulkan/vma_impl.cpp
        Engine/Source/Renderer/Particles/ParticleSystem.cpp
        Engine/Source/Renderer/Geometry/Terrain.cpp
        Platform/SDL/SDLInput.cpp
)
gameengine_enable_warnings(Engine)
gameengine_enable_sanitizers(Engine)

target_link_libraries(Engine
        PUBLIC SDL3::SDL3 Vulkan::Vulkan
        PRIVATE DearImGui physx_lib)
target_include_directories(Engine
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Engine/Include>
            ${GLM_INCLUDE_DIR}
            ${VMA_INCLUDE_DIR}
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Source
            ${CMAKE_CURRENT_SOURCE_DIR}/ThridParty/stb
)

add_executable(Editor
        Editor/Source/main.cpp
        Editor/Source/EditorUi.cpp
        Editor/Source/Panels/EditorStyle.cpp
        Editor/Source/Panels/EditorSceneSession.cpp
        Editor/Source/Panels/AssetManagerPanel.cpp)
gameengine_enable_warnings(Editor)
gameengine_enable_sanitizers(Editor)
target_include_directories(Editor PRIVATE Editor/Include)
target_link_libraries(Editor PRIVATE DearImGui Engine SDL3::SDL3 Vulkan::Vulkan)
gameengine_copy_runtime_dependencies(Editor)
target_compile_definitions(Editor PRIVATE GAMEENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

find_program(GLSLANG_VALIDATOR NAMES glslangValidator)
find_program(GLSLC NAMES glslc)

set(SHADER_OUT_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT_DIR})
set(SHADER_OUTPUTS)

set(SHADER_SOURCES
        Assets/Shaders/Library/pbr.vert
        Assets/Shaders/Library/pbr.frag
        Assets/Shaders/Library/outline.vert
        Assets/Shaders/Library/outline.frag
        Assets/Shaders/shadow.vert
        Assets/Shaders/shadow.frag
        Assets/Shaders/skybox.vert
        Assets/Shaders/skybox.frag
        Assets/Shaders/tonemap.vert
        Assets/Shaders/tonemap.frag
        Assets/Shaders/Culling/hiz_copy.comp
        Assets/Shaders/Culling/hiz_reduce.comp
        Assets/Shaders/Culling/hiz_cull.comp)
list(APPEND SHADER_SOURCES
        Assets/Shaders/Particles/particle_update.comp
        Assets/Shaders/Particles/particle.vert
        Assets/Shaders/Particles/particle.frag
        Assets/Shaders/UI/ui.vert
        Assets/Shaders/UI/ui.frag)
foreach(SHADER_SOURCE IN LISTS SHADER_SOURCES)
    get_filename_component(SHADER_NAME ${SHADER_SOURCE} NAME)
    list(APPEND SHADER_OUTPUTS ${SHADER_OUT_DIR}/${SHADER_NAME}.spv)
    if(GLSLC)
        add_custom_command(OUTPUT ${SHADER_OUT_DIR}/${SHADER_NAME}.spv
                COMMAND ${GLSLC} ${CMAKE_SOURCE_DIR}/${SHADER_SOURCE} -o ${SHADER_OUT_DIR}/${SHADER_NAME}.spv
                DEPENDS ${CMAKE_SOURCE_DIR}/${SHADER_SOURCE})
    elseif(GLSLANG_VALIDATOR)
        add_custom_command(OUTPUT ${SHADER_OUT_DIR}/${SHADER_NAME}.spv
                COMMAND ${GLSLANG_VALIDATOR} -V ${CMAKE_SOURCE_DIR}/${SHADER_SOURCE} -o ${SHADER_OUT_DIR}/${SHADER_NAME}.spv
                DEPENDS ${CMAKE_SOURCE_DIR}/${SHADER_SOURCE})
    else()
        message(FATAL_ERROR "glslc not found install Vulkan SDK.")
    endif()
endforeach()

add_custom_target(Shaders DEPENDS ${SHADER_OUTPUTS})

if(BUILD_TESTING)
    include(FetchContent)
    FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.17.0
            GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(googletest)

    add_executable(GamEngineTests
            Tests/MathTests.cpp
            Tests/EcsTests.cpp
            Tests/GeneralTests.cpp
            Tests/ComponentTests.cpp
            Tests/UiAndLayoutTests.cpp
            Tests/TimeTests.cpp
            Tests/SceneBuilderTests.cpp
            Engine/Source/Core/Camera.cpp
            Engine/Source/Core/Time.cpp
            Engine/Source/Renderer/Geometry/Terrain.cpp
            Engine/Source/UI/RectTransform.cpp)
    target_link_libraries(GamEngineTests PRIVATE GTest::gtest_main)
    target_include_directories(GamEngineTests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Include
            ${GLM_INCLUDE_DIR})
    gameengine_enable_warnings(GamEngineTests)
    gameengine_enable_sanitizers(GamEngineTests)

    include(GoogleTest)
    gtest_discover_tests(GamEngineTests)

    add_executable(PhysicsMeshTests
            Tests/PhysicsMeshTests.cpp
            Tests/PhysXPhysicsTests.cpp)
    target_link_libraries(PhysicsMeshTests PRIVATE Engine GTest::gtest_main)
    gameengine_enable_warnings(PhysicsMeshTests)
    gameengine_enable_sanitizers(PhysicsMeshTests)
    gameengine_copy_runtime_dependencies(PhysicsMeshTests)
    gtest_discover_tests(PhysicsMeshTests)
endif()

add_subdirectory(Sandbox)
# Play in the editor starts this executable.  Keeping it as a build dependency
# means an Editor build always leaves a runnable game beside its assets.
add_dependencies(Editor GamEngine)
