cmake_minimum_required(VERSION 4.4)
project(GamEngine)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Vulkan REQUIRED)
set(SDL3_DIR "$ENV{VULKAN_SDK}/cmake")
find_package(SDL3 CONFIG REQUIRED)
find_path(GLM_INCLUDE_DIR NAMES glm/glm.hpp HINTS "$ENV{VULKAN_SDK}/Include" REQUIRED)

add_library(Engine
        Engine/Source/Core/camera.cpp
        Engine/Source/Core/time.cpp
        Engine/Source/ECS/Registry.cpp
        Engine/Source/Renderer/shader_loader.cpp
        Engine/Source/Renderer/Textures/Cubemap.cpp
        Engine/Source/Renderer/Skybox/Skybox.cpp
        Engine/Source/Renderer/Skybox/SkyboxPipeline.cpp
        Engine/Source/Renderer/Vulkan/buffer.cpp
        Engine/Source/Renderer/Vulkan/depth_buffer.cpp
        Engine/Source/Renderer/Vulkan/graphics_pipeline.cpp
        Engine/Source/Renderer/Vulkan/msaa.cpp
        Engine/Source/Renderer/Vulkan/renderer.cpp
        Engine/Source/Renderer/Vulkan/shadow_map.cpp
        Engine/Source/Renderer/Vulkan/swapchain.cpp
        Engine/Source/Renderer/Vulkan/vulkan_device.cpp
)

target_link_libraries(Engine PUBLIC SDL3::SDL3 Vulkan::Vulkan)
target_include_directories(Engine
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Engine/Include>
            ${GLM_INCLUDE_DIR}
)

add_executable(GamEngine Sandbox/Source/main.cpp)
target_link_libraries(GamEngine PRIVATE Engine)

find_program(GLSLANG_VALIDATOR NAMES glslangValidator)
find_program(GLSLC NAMES glslc)

set(SHADER_OUT_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT_DIR})

if(GLSLC)
    add_custom_command(
            OUTPUT ${SHADER_OUT_DIR}/vert.spv
            COMMAND ${GLSLC} ${CMAKE_SOURCE_DIR}/Assets/Shaders/triangle.vert -o ${SHADER_OUT_DIR}/vert.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/Assets/Shaders/triangle.vert
    )
    add_custom_command(
            OUTPUT ${SHADER_OUT_DIR}/shadow.vert.spv
            COMMAND ${GLSLC} ${CMAKE_SOURCE_DIR}/Assets/Shaders/shadow.vert -o ${SHADER_OUT_DIR}/shadow.vert.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/Assets/Shaders/shadow.vert
    )
    add_custom_command(
            OUTPUT ${SHADER_OUT_DIR}/frag.spv
            COMMAND ${GLSLC} ${CMAKE_SOURCE_DIR}/Assets/Shaders/triangle.frag -o ${SHADER_OUT_DIR}/frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/Assets/Shaders/triangle.frag
    )
    add_custom_command(
            OUTPUT ${SHADER_OUT_DIR}/skybox.vert.spv
            COMMAND ${GLSLC} ${CMAKE_SOURCE_DIR}/Assets/Shaders/skybox.vert -o ${SHADER_OUT_DIR}/skybox.vert.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/Assets/Shaders/skybox.vert
    )
    add_custom_command(
            OUTPUT ${SHADER_OUT_DIR}/skybox.frag.spv
            COMMAND ${GLSLC} ${CMAKE_SOURCE_DIR}/Assets/Shaders/skybox.frag -o ${SHADER_OUT_DIR}/skybox.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/Assets/Shaders/skybox.frag
    )
elseif(GLSLANG_VALIDATOR)
    add_custom_command(
            OUTPUT ${SHADER_OUT_DIR}/vert.spv
            COMMAND ${GLSLANG_VALIDATOR} -V ${CMAKE_SOURCE_DIR}/Assets/Shaders/triangle.vert -o ${SHADER_OUT_DIR}/vert.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/Assets/Shaders/triangle.vert
    )
    add_custom_command(
            OUTPUT ${SHADER_OUT_DIR}/frag.spv
            COMMAND ${GLSLANG_VALIDATOR} -V ${CMAKE_SOURCE_DIR}/Assets/Shaders/triangle.frag -o ${SHADER_OUT_DIR}/frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/Assets/Shaders/triangle.frag
    )
    add_custom_command(
            OUTPUT ${SHADER_OUT_DIR}/shadow.vert.spv
            COMMAND ${GLSLANG_VALIDATOR} -V ${CMAKE_SOURCE_DIR}/Assets/Shaders/shadow.vert -o ${SHADER_OUT_DIR}/shadow.vert.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/Assets/Shaders/shadow.vert
    )
    add_custom_command(
            OUTPUT ${SHADER_OUT_DIR}/skybox.vert.spv
            COMMAND ${GLSLANG_VALIDATOR} -V ${CMAKE_SOURCE_DIR}/Assets/Shaders/skybox.vert -o ${SHADER_OUT_DIR}/skybox.vert.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/Assets/Shaders/skybox.vert
    )
    add_custom_command(
            OUTPUT ${SHADER_OUT_DIR}/skybox.frag.spv
            COMMAND ${GLSLANG_VALIDATOR} -V ${CMAKE_SOURCE_DIR}/Assets/Shaders/skybox.frag -o ${SHADER_OUT_DIR}/skybox.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/Assets/Shaders/skybox.frag
    )
else()
    message(FATAL_ERROR "glslc not found install Vulkan SDK.")
endif()

add_custom_target(Shaders DEPENDS ${SHADER_OUT_DIR}/vert.spv ${SHADER_OUT_DIR}/frag.spv ${SHADER_OUT_DIR}/shadow.vert.spv ${SHADER_OUT_DIR}/skybox.vert.spv ${SHADER_OUT_DIR}/skybox.frag.spv)
add_dependencies(GamEngine Shaders)
