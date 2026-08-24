cmake_minimum_required(VERSION 4.4)
project(GamEngine)

include(CTest)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Vulkan 1.4 REQUIRED)
set(SDL3_DIR "$ENV{VULKAN_SDK}/cmake")
find_package(SDL3 CONFIG REQUIRED)

include(FetchContent)
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
        Engine/Source/Assets/Content.cpp
        Engine/Source/Core/camera.cpp
        Engine/Source/Core/time.cpp
        Engine/Source/ECS/Registry.cpp
        Engine/Source/Input/Input.cpp
        Engine/Source/Scene/SceneSerializer.cpp
        Engine/Source/Scene/Scene.cpp
        Engine/Source/Scene/Actor.cpp
        Engine/Source/Scene/Prefab.cpp
        Engine/Source/Scene/ScenePresets.cpp
        Engine/Source/Physics/PhysicsSystem.cpp
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
        Engine/Source/Renderer/Vulkan/CameraController.cpp
        Engine/Source/Renderer/Vulkan/ViewportRenderTarget.cpp
        Engine/Source/Renderer/Vulkan/shadow_map.cpp
        Engine/Source/Renderer/Vulkan/swapchain.cpp
        Engine/Source/Renderer/Vulkan/vulkan_device.cpp
        Engine/Source/Renderer/Vulkan/vma_impl.cpp
        Engine/Source/Renderer/Particles/ParticleSystem.cpp
        Platform/SDL/SDLInput.cpp
)

target_link_libraries(Engine PUBLIC SDL3::SDL3 Vulkan::Vulkan PRIVATE DearImGui)
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
        Editor/Source/Panels/EditorStyle.cpp
        Editor/Source/Panels/EditorSceneSession.cpp)
target_include_directories(Editor PRIVATE Editor/Include)
target_link_libraries(Editor PRIVATE DearImGui Engine SDL3::SDL3 Vulkan::Vulkan)
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
add_subdirectory(Sandbox)
# Play in the editor starts this executable.  Keeping it as a build dependency
# means an Editor build always leaves a runnable game beside its assets.
add_dependencies(Editor GamEngine)
if(BUILD_TESTING)
    add_executable(CanvasTests Tests/CanvasTests.cpp)
    target_link_libraries(CanvasTests PRIVATE Engine)
    add_test(NAME CanvasTests COMMAND CanvasTests)

    add_executable(SceneSerializerTests Tests/SceneSerializerTests.cpp)
    target_link_libraries(SceneSerializerTests PRIVATE Engine)
    add_test(NAME SceneSerializerTests COMMAND SceneSerializerTests)

    add_executable(RegistryTests Tests/RegistryTests.cpp)
    target_link_libraries(RegistryTests PRIVATE Engine)
    add_test(NAME RegistryTests COMMAND RegistryTests)

    add_executable(AssetManagerTests Tests/AssetManagerTests.cpp)
    target_link_libraries(AssetManagerTests PRIVATE Engine)
    add_test(NAME AssetManagerTests COMMAND AssetManagerTests)

    add_executable(ScriptSystemTests Tests/ScriptSystemTests.cpp)
    target_link_libraries(ScriptSystemTests PRIVATE Engine)
    add_test(NAME ScriptSystemTests COMMAND ScriptSystemTests)

    add_executable(PhysicsSystemTests Tests/PhysicsSystemTests.cpp)
    target_link_libraries(PhysicsSystemTests PRIVATE Engine)
    add_test(NAME PhysicsSystemTests COMMAND PhysicsSystemTests)

    add_executable(ActorTests Tests/ActorTests.cpp)
    target_link_libraries(ActorTests PRIVATE Engine)
    add_test(NAME ActorTests COMMAND ActorTests)

    add_executable(MathTests Tests/MathTests.cpp)
    target_link_libraries(MathTests PRIVATE Engine)
    add_test(NAME MathTests COMMAND MathTests)

    add_executable(GeometryTests Tests/GeometryTests.cpp)
    target_link_libraries(GeometryTests PRIVATE Engine)
    add_test(NAME GeometryTests COMMAND GeometryTests)

    add_executable(UITests Tests/UITests.cpp)
    target_link_libraries(UITests PRIVATE Engine)
    add_test(NAME UITests COMMAND UITests)

    add_executable(SceneTests Tests/SceneTests.cpp)
    target_link_libraries(SceneTests PRIVATE Engine)
    add_test(NAME SceneTests COMMAND SceneTests)

    add_executable(AssetCacheTests Tests/AssetCacheTests.cpp)
    target_link_libraries(AssetCacheTests PRIVATE Engine)
    add_test(NAME AssetCacheTests COMMAND AssetCacheTests)

    add_executable(ScriptRegistryTests Tests/ScriptRegistryTests.cpp)
    target_link_libraries(ScriptRegistryTests PRIVATE Engine)
    add_test(NAME ScriptRegistryTests COMMAND ScriptRegistryTests)

    add_executable(PhysicsEdgeTests Tests/PhysicsEdgeTests.cpp)
    target_link_libraries(PhysicsEdgeTests PRIVATE Engine)
    add_test(NAME PhysicsEdgeTests COMMAND PhysicsEdgeTests)

    add_executable(SceneIOTests Tests/SceneIOTests.cpp)
    target_link_libraries(SceneIOTests PRIVATE Engine)
    add_test(NAME SceneIOTests COMMAND SceneIOTests)

    add_executable(ContentTests Tests/ContentTests.cpp)
    target_link_libraries(ContentTests PRIVATE Engine)
    add_test(NAME ContentTests COMMAND ContentTests)

    add_executable(InputTests Tests/InputTests.cpp)
    target_link_libraries(InputTests PRIVATE Engine)
    add_test(NAME InputTests COMMAND InputTests)

    add_executable(PrefabTests Tests/PrefabTests.cpp)
    target_link_libraries(PrefabTests PRIVATE Engine)
    add_test(NAME PrefabTests COMMAND PrefabTests)

    add_executable(TransformTests Tests/TransformTests.cpp)
    target_link_libraries(TransformTests PRIVATE Engine)
    add_test(NAME TransformTests COMMAND TransformTests)

    add_executable(InterfaceTests Tests/InterfaceTests.cpp)
    target_link_libraries(InterfaceTests PRIVATE Engine)
    add_test(NAME InterfaceTests COMMAND InterfaceTests)

    add_executable(EntityTests Tests/EntityTests.cpp)
    target_link_libraries(EntityTests PRIVATE Engine)
    add_test(NAME EntityTests COMMAND EntityTests)

    add_executable(FrustumTests Tests/FrustumTests.cpp)
    target_link_libraries(FrustumTests PRIVATE Engine)
    add_test(NAME FrustumTests COMMAND FrustumTests)

    add_executable(RendererComponentTests Tests/RendererComponentTests.cpp)
    target_link_libraries(RendererComponentTests PRIVATE Engine)
    add_test(NAME RendererComponentTests COMMAND RendererComponentTests)

    add_executable(TextTests Tests/TextTests.cpp)
    target_link_libraries(TextTests PRIVATE Engine)
    add_test(NAME TextTests COMMAND TextTests)

    add_executable(FontAtlasTests Tests/FontAtlasTests.cpp)
    target_link_libraries(FontAtlasTests PRIVATE Engine)
    add_test(NAME FontAtlasTests COMMAND FontAtlasTests)

    add_executable(ParticleComponentTests Tests/ParticleComponentTests.cpp)
    target_link_libraries(ParticleComponentTests PRIVATE Engine)
    add_test(NAME ParticleComponentTests COMMAND ParticleComponentTests)

    add_executable(ComponentDefaultsTests Tests/ComponentDefaultsTests.cpp)
    target_link_libraries(ComponentDefaultsTests PRIVATE Engine)
    add_test(NAME ComponentDefaultsTests COMMAND ComponentDefaultsTests)

    add_executable(UUIDTests Tests/UUIDTests.cpp)
    target_link_libraries(UUIDTests PRIVATE Engine)
    add_test(NAME UUIDTests COMMAND UUIDTests)

    add_executable(VectorTests Tests/VectorTests.cpp)
    target_link_libraries(VectorTests PRIVATE Engine)
    add_test(NAME VectorTests COMMAND VectorTests)

    add_executable(SceneEditorTests Tests/SceneEditorTests.cpp)
    target_link_libraries(SceneEditorTests PRIVATE Engine)
    add_test(NAME SceneEditorTests COMMAND SceneEditorTests)

    add_executable(ParticleSceneTests Tests/ParticleSceneTests.cpp)
    target_link_libraries(ParticleSceneTests PRIVATE Engine)
    add_test(NAME ParticleSceneTests COMMAND ParticleSceneTests)
endif()
