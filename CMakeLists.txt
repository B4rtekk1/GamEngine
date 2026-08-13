cmake_minimum_required(VERSION 4.4)
project(GamEngine)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Vulkan 1.4 REQUIRED)
set(SDL3_DIR "$ENV{VULKAN_SDK}/cmake")
find_package(SDL3 CONFIG REQUIRED)
find_path(GLM_INCLUDE_DIR NAMES glm/glm.hpp HINTS "$ENV{VULKAN_SDK}/Include" REQUIRED)

add_library(Engine
        Engine/Source/Core/camera.cpp
        Engine/Source/Core/time.cpp
        Engine/Source/ECS/Registry.cpp
        Engine/Source/Input/Input.cpp
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
        Engine/Source/Renderer/Vulkan/shadow_map.cpp
        Engine/Source/Renderer/Vulkan/swapchain.cpp
        Engine/Source/Renderer/Vulkan/vulkan_device.cpp
        Platform/SDL/SDLInput.cpp
)

target_link_libraries(Engine PUBLIC SDL3::SDL3 Vulkan::Vulkan)
target_include_directories(Engine
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Engine/Include>
            ${GLM_INCLUDE_DIR}
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Source
)

add_executable(GamEngine Sandbox/Source/main.cpp)
target_link_libraries(GamEngine PRIVATE Engine)

find_program(GLSLANG_VALIDATOR NAMES glslangValidator)
find_program(GLSLC NAMES glslc)

set(SHADER_OUT_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT_DIR})

set(SHADER_SOURCES
        Assets/Shaders/Library/pbr.vert
        Assets/Shaders/Library/pbr.frag
        Assets/Shaders/shadow.vert
        Assets/Shaders/skybox.vert
        Assets/Shaders/skybox.frag
        Assets/Shaders/tonemap.vert
        Assets/Shaders/tonemap.frag
        Assets/Shaders/Culling/hiz_copy.comp
        Assets/Shaders/Culling/hiz_reduce.comp
        Assets/Shaders/Culling/hiz_cull.comp)
foreach(SHADER_SOURCE IN LISTS SHADER_SOURCES)
    get_filename_component(SHADER_NAME ${SHADER_SOURCE} NAME)
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

add_custom_target(Shaders DEPENDS ${SHADER_OUT_DIR}/pbr.vert.spv ${SHADER_OUT_DIR}/pbr.frag.spv ${SHADER_OUT_DIR}/shadow.vert.spv ${SHADER_OUT_DIR}/skybox.vert.spv ${SHADER_OUT_DIR}/skybox.frag.spv ${SHADER_OUT_DIR}/tonemap.vert.spv ${SHADER_OUT_DIR}/tonemap.frag.spv ${SHADER_OUT_DIR}/hiz_copy.comp.spv ${SHADER_OUT_DIR}/hiz_reduce.comp.spv ${SHADER_OUT_DIR}/hiz_cull.comp.spv)
add_dependencies(GamEngine Shaders)
add_custom_command(TARGET GamEngine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:GamEngine>/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${SHADER_OUT_DIR}" "$<TARGET_FILE_DIR:GamEngine>/shaders"
        COMMENT "Copying SPIR-V shaders beside GamEngine"
)
