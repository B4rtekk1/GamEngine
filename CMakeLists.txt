cmake_minimum_required(VERSION 4.4)
project(GamEngine)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Vulkan REQUIRED)
set(SDL3_DIR "$ENV{VULKAN_SDK}/cmake")
find_package(SDL3 CONFIG REQUIRED)
find_path(GLM_INCLUDE_DIR NAMES glm/glm.hpp HINTS "$ENV{VULKAN_SDK}/Include" REQUIRED)

add_executable(GamEngine
        src/main.cpp
        src/vulkan/msaa.cpp
        src/vulkan/msaa.h
        src/utils/shader_loader.cpp
        src/utils/shader_loader.h
        src/vulkan/vulkan_device.h
        src/vulkan/vulkan_device.cpp
        src/vulkan/swapchain.cpp
        src/vulkan/swapchain.h
)

target_link_libraries(GamEngine PRIVATE SDL3::SDL3 Vulkan::Vulkan)
target_include_directories(GamEngine PRIVATE ${GLM_INCLUDE_DIR})

find_program(GLSLANG_VALIDATOR NAMES glslangValidator)
find_program(GLSLC NAMES glslc)

set(SHADER_OUT_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT_DIR})

if(GLSLC)
    add_custom_command(
            OUTPUT ${SHADER_OUT_DIR}/vert.spv
            COMMAND ${GLSLC} ${CMAKE_SOURCE_DIR}/src/shaders/triangle.vert -o ${SHADER_OUT_DIR}/vert.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/src/shaders/triangle.vert
    )
    add_custom_command(
            OUTPUT ${SHADER_OUT_DIR}/frag.spv
            COMMAND ${GLSLC} ${CMAKE_SOURCE_DIR}/src/shaders/triangle.frag -o ${SHADER_OUT_DIR}/frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/src/shaders/triangle.frag
    )
elseif(GLSLANG_VALIDATOR)
    add_custom_command(
            OUTPUT ${SHADER_OUT_DIR}/vert.spv
            COMMAND ${GLSLANG_VALIDATOR} -V ${CMAKE_SOURCE_DIR}/src/shaders/triangle.vert -o ${SHADER_OUT_DIR}/vert.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/src/shaders/triangle.vert
    )
    add_custom_command(
            OUTPUT ${SHADER_OUT_DIR}/frag.spv
            COMMAND ${GLSLANG_VALIDATOR} -V ${CMAKE_SOURCE_DIR}/src/shaders/triangle.frag -o ${SHADER_OUT_DIR}/frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/src/shaders/triangle.frag
    )
else()
    message(FATAL_ERROR "glslc not found install Vulkan SDK.")
endif()

add_custom_target(Shaders DEPENDS ${SHADER_OUT_DIR}/vert.spv ${SHADER_OUT_DIR}/frag.spv)
add_dependencies(GamEngine Shaders)
