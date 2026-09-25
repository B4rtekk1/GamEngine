add_library(Engine SHARED
    Source/Application.cpp Source/Project.cpp Source/Assets/StbImage.cpp Source/Assets/AssetManager.cpp Source/Assets/Gtex.cpp Source/Assets/Gmesh.cpp Source/Assets/CookCache.cpp Source/Assets/TextureCooker.cpp Source/Assets/GlbLoader.cpp Source/Assets/Content.cpp
    Source/Core/Camera.cpp Source/Core/Diagnostics.cpp Source/Core/Profiler.cpp Source/Core/TaskScheduler.cpp Source/Core/Time.cpp Source/ECS/Registry.cpp Source/Input/Input.cpp Source/Input/InputMap.cpp
    Source/Scene/SceneSerializer.cpp Source/Scene/SceneRegistrySerializer.cpp Source/Scene/Scene.cpp Source/Scene/Actor.cpp Source/Scene/TransformSystem.cpp Source/Scene/Prefab.cpp Source/Scene/ScenePresets.cpp
    Source/Physics/PhysXPhysicsSystem.cpp Source/Scripting/ScriptSystem.cpp Source/Scripting/Script.cpp Source/Scripting/ScriptRegistry.cpp Source/Scripting/DynamicLibrary.cpp Source/Scripting/ScriptModuleManager.cpp
    Source/UI/Canvas.cpp Source/UI/CanvasRenderer.cpp Source/UI/UIBatch.cpp Source/UI/Vulkan/UIFontAtlas.cpp Source/UI/Vulkan/UIFontAtlasRuntime.cpp Source/UI/RectTransform.cpp Source/UI/Vulkan/UIPipeline.cpp
    Source/Renderer/shader_loader.cpp Source/Renderer/Textures/Cubemap.cpp Source/Renderer/Textures/Texture2D.cpp Source/Renderer/Textures/VirtualTexture.cpp Source/Renderer/Skybox/Skybox.cpp Source/Renderer/Skybox/SkyboxPipeline.cpp Source/Renderer/Lighting/LightingBuffer.cpp Source/Renderer/Lighting/EnvironmentBaker.cpp Source/Renderer/Lighting/ImageBasedLighting.cpp Source/Renderer/Lighting/ReflectionProbeManager.cpp Source/Renderer/Lighting/ReflectionProbeCapture.cpp
    Source/Renderer/ShaderGraph/ShaderGraphCompiler.cpp Source/Renderer/ShaderGraph/ShaderGraphVulkan.cpp Source/Renderer/ShaderGraph/ShaderNodeFactory.cpp Source/Renderer/ShaderGraph/ShaderNodeRegistry.cpp Source/Renderer/ShaderGraph/ShaderGraphSerializer.cpp
    Source/Renderer/Passes/ForwardPass.cpp Source/Renderer/Passes/ShadowPass.cpp Source/Renderer/Passes/SkyPass.cpp Source/Renderer/Passes/TonemapPass.cpp Source/Renderer/Passes/BloomPass.cpp Source/Renderer/Passes/GtaoPass.cpp Source/Renderer/Passes/TemporalAaPass.cpp Source/Renderer/Passes/RtContactShadowPass.cpp Source/Renderer/Passes/DirectionalVisibilityPass.cpp Source/Renderer/RayTracing/AccelerationStructureManager.cpp
    Source/Renderer/RenderGraph/RenderGraph.cpp Source/Renderer/Lighting/DDGI/DDGISystem.cpp
    Source/Renderer/Culling/GPUCullingPass.cpp Source/Renderer/Culling/GPUInstanceCullingPass.cpp Source/Renderer/Culling/MeshletCulling.cpp Source/Renderer/Culling/IndexedIndirectDrawCount.cpp Source/Renderer/Culling/HiZBuffer.cpp Source/Renderer/Culling/HiZPass.cpp Source/Renderer/GPUSceneDatabase.cpp
    Source/Renderer/Vulkan/buffer.cpp Source/Renderer/Vulkan/upload_context.cpp Source/Renderer/Vulkan/depth_buffer.cpp Source/Renderer/Vulkan/graphics_pipeline.cpp Source/Renderer/Vulkan/hdr_buffer.cpp Source/Renderer/Vulkan/msaa.cpp Source/Renderer/Vulkan/renderer.cpp Source/Renderer/Vulkan/renderer_scene_helpers.cpp Source/Renderer/Vulkan/CameraController.cpp Source/Renderer/Vulkan/ViewportRenderTarget.cpp Source/Renderer/Vulkan/GpuTimestampProfiler.cpp Source/Renderer/Vulkan/MemoryBudgetManager.cpp Source/Renderer/Vulkan/shadow_map.cpp Source/Renderer/Vulkan/swapchain.cpp Source/Renderer/Vulkan/vulkan_device.cpp Source/Renderer/Vulkan/vma_impl.cpp
    Source/Renderer/Particles/ParticleSystem.cpp Source/Renderer/Geometry/Terrain.cpp Source/Renderer/Geometry/ProceduralCloud.cpp Source/Renderer/Geometry/Meshlet.cpp Source/Renderer/Water/WaterSystem.cpp Source/Renderer/Water/VirtualWaterPageBuilder.cpp Source/Renderer/Water/WaterRenderWorld.cpp Source/Renderer/Water/VirtualWaterRenderer.cpp Source/Renderer/Passes/WaterPass.cpp
    ${PROJECT_SOURCE_DIR}/Platform/SDL/SDLInput.cpp)

set(GE_BC7_BACKEND "auto" CACHE STRING "BC7 encoder: auto, bc7e, or compressonator")
set_property(CACHE GE_BC7_BACKEND PROPERTY STRINGS auto bc7e compressonator)
set(GE_BC7E_PROFILE "basic" CACHE STRING "BC7E quality: veryfast, fast, basic, or slow")
set_property(CACHE GE_BC7E_PROFILE PROPERTY STRINGS veryfast fast basic slow)
if(NOT GE_BC7_BACKEND MATCHES "^(auto|bc7e|compressonator)$")
    message(FATAL_ERROR "Unknown GE_BC7_BACKEND: ${GE_BC7_BACKEND}")
endif()
if(NOT GE_BC7E_PROFILE MATCHES "^(veryfast|fast|basic|slow)$")
    message(FATAL_ERROR "Unknown GE_BC7E_PROFILE: ${GE_BC7E_PROFILE}")
endif()

set(GAMEENGINE_BC7E_ENABLED 0)
set(GAMEENGINE_BC7_ENCODER "compressonator")
if(NOT GE_BC7_BACKEND STREQUAL "compressonator")
    include("${PROJECT_SOURCE_DIR}/cmake/ISPC.cmake")
    gameengine_fetch_ispc(GAMEENGINE_ISPC_EXECUTABLE)
    set(GAMEENGINE_BC7E_ENABLED 1)
endif()

if(GAMEENGINE_BC7E_ENABLED)
    include(FetchContent)
    FetchContent_Declare(Bc7eSource
        GIT_REPOSITORY https://github.com/richgel999/bc7enc_rdo.git
        GIT_TAG master
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR cmake-not-used)
    FetchContent_MakeAvailable(Bc7eSource)
    set(BC7E_SOURCE "${bc7esource_SOURCE_DIR}/bc7e.ispc")
    set(BC7E_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/bc7e")
    set(BC7E_HEADER "${BC7E_OUTPUT_DIR}/bc7e_ispc.h")
    if(MSVC)
        set(BC7E_OBJECT "${BC7E_OUTPUT_DIR}/bc7e.obj")
        set(BC7E_VARIANTS "${BC7E_OUTPUT_DIR}/bc7e_sse4.obj" "${BC7E_OUTPUT_DIR}/bc7e_avx2.obj")
    else()
        set(BC7E_OBJECT "${BC7E_OUTPUT_DIR}/bc7e.o")
        set(BC7E_VARIANTS "${BC7E_OUTPUT_DIR}/bc7e_sse4.o" "${BC7E_OUTPUT_DIR}/bc7e_avx2.o")
        set(BC7E_PLATFORM_FLAGS --pic)
    endif()
    add_custom_command(OUTPUT "${BC7E_OBJECT}" "${BC7E_HEADER}"
        BYPRODUCTS ${BC7E_VARIANTS}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${BC7E_OUTPUT_DIR}"
        COMMAND "${GAMEENGINE_ISPC_EXECUTABLE}" "${BC7E_SOURCE}"
            -O2 -o "${BC7E_OBJECT}" -h "${BC7E_HEADER}"
            --target=sse4.2-i32x4,avx2-i32x8 --opt=fast-math --opt=disable-assertions
            ${BC7E_PLATFORM_FLAGS}
        DEPENDS "${BC7E_SOURCE}"
        VERBATIM)
    add_custom_target(BC7ESimd DEPENDS "${BC7E_OBJECT}" "${BC7E_HEADER}")
    add_dependencies(Engine BC7ESimd)
    set_source_files_properties("${BC7E_OBJECT}" ${BC7E_VARIANTS}
        PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
    target_sources(Engine PRIVATE "${BC7E_OBJECT}" ${BC7E_VARIANTS})
    target_include_directories(Engine PRIVATE "${BC7E_OUTPUT_DIR}")
    execute_process(COMMAND git -C "${bc7esource_SOURCE_DIR}" rev-parse HEAD
        OUTPUT_VARIABLE BC7E_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    set(GAMEENGINE_BC7_ENCODER "bc7e-${BC7E_REVISION}-${GE_BC7E_PROFILE}")
endif()

if(GE_BC7E_PROFILE STREQUAL "veryfast")
    set(GAMEENGINE_BC7E_PROFILE 0)
elseif(GE_BC7E_PROFILE STREQUAL "fast")
    set(GAMEENGINE_BC7E_PROFILE 1)
elseif(GE_BC7E_PROFILE STREQUAL "basic")
    set(GAMEENGINE_BC7E_PROFILE 2)
else()
    set(GAMEENGINE_BC7E_PROFILE 3)
endif()
message(STATUS "GamEngine BC7 encoder: ${GAMEENGINE_BC7_ENCODER}")
file(GLOB_RECURSE ENGINE_PUBLIC_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Include/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/Include/*.hpp")
target_sources(Engine PRIVATE FILE_SET engine_public_headers TYPE HEADERS BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/Include" FILES ${ENGINE_PUBLIC_HEADERS})
target_link_libraries(Engine PUBLIC SDL3::SDL3 Vulkan::Vulkan PRIVATE physx_lib CMP_Core GameEngineTinyExr)
target_compile_definitions(Engine PRIVATE PX_PHYSX_STATIC_LIB
    GAMEENGINE_BC7E_ENABLED=${GAMEENGINE_BC7E_ENABLED}
    GAMEENGINE_BC7E_PROFILE=${GAMEENGINE_BC7E_PROFILE}
    GAMEENGINE_BC7_ENCODER="${GAMEENGINE_BC7_ENCODER}"
    GAMEENGINE_COMPRESSONATOR_REVISION="${GAMEENGINE_COMPRESSONATOR_REVISION}"
    GAMEENGINE_SLANGC_PATH="${GAMEENGINE_SLANGC}")
target_include_directories(Engine PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include> ${GLM_INCLUDE_DIR} ${VMA_INCLUDE_DIR} PRIVATE "${PROJECT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/Source" "${PROJECT_SOURCE_DIR}/ThridParty/stb")
gameengine_enable_warnings(Engine)
gameengine_enable_sanitizers(Engine)
if(WIN32)
    set_target_properties(Engine PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()
# CMake discovers DLL exports by scanning individual COFF objects. MSVC /GL
# produces LTCG intermediate objects, which the export scanner cannot read.
gameengine_enable_release_ipo(Engine)
