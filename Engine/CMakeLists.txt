add_library(Engine SHARED
    Source/Application.cpp Source/Project.cpp Source/Assets/StbImage.cpp Source/Assets/AssetManager.cpp Source/Assets/Gtex.cpp Source/Assets/Gmesh.cpp Source/Assets/TextureCooker.cpp Source/Assets/GlbLoader.cpp Source/Assets/Content.cpp
    Source/Core/Camera.cpp Source/Core/Diagnostics.cpp Source/Core/Profiler.cpp Source/Core/TaskScheduler.cpp Source/Core/Time.cpp Source/ECS/Registry.cpp Source/Input/Input.cpp Source/Input/InputMap.cpp
    Source/Scene/SceneSerializer.cpp Source/Scene/SceneRegistrySerializer.cpp Source/Scene/Scene.cpp Source/Scene/Actor.cpp Source/Scene/TransformSystem.cpp Source/Scene/Prefab.cpp Source/Scene/ScenePresets.cpp
    Source/Physics/PhysXPhysicsSystem.cpp Source/Scripting/ScriptSystem.cpp Source/Scripting/Script.cpp Source/Scripting/ScriptRegistry.cpp Source/Scripting/DynamicLibrary.cpp Source/Scripting/ScriptModuleManager.cpp
    Source/UI/Canvas.cpp Source/UI/CanvasRenderer.cpp Source/UI/UIBatch.cpp Source/UI/Vulkan/UIFontAtlas.cpp Source/UI/Vulkan/UIFontAtlasRuntime.cpp Source/UI/RectTransform.cpp Source/UI/Vulkan/UIPipeline.cpp
    Source/Renderer/shader_loader.cpp Source/Renderer/Textures/Cubemap.cpp Source/Renderer/Textures/Texture2D.cpp Source/Renderer/Skybox/Skybox.cpp Source/Renderer/Skybox/SkyboxPipeline.cpp Source/Renderer/Lighting/LightingBuffer.cpp Source/Renderer/Lighting/ImageBasedLighting.cpp Source/Renderer/Lighting/ReflectionProbeManager.cpp Source/Renderer/Lighting/ReflectionProbeCapture.cpp
    Source/Renderer/ShaderGraph/ShaderGraphCompiler.cpp Source/Renderer/ShaderGraph/ShaderGraphVulkan.cpp Source/Renderer/ShaderGraph/ShaderNodeFactory.cpp Source/Renderer/ShaderGraph/ShaderNodeRegistry.cpp Source/Renderer/ShaderGraph/ShaderGraphSerializer.cpp
    Source/Renderer/Passes/ForwardPass.cpp Source/Renderer/Passes/ShadowPass.cpp Source/Renderer/Passes/SkyPass.cpp Source/Renderer/Passes/TonemapPass.cpp Source/Renderer/Passes/BloomPass.cpp Source/Renderer/Passes/TemporalAaPass.cpp
    Source/Renderer/RenderGraph/RenderGraph.cpp
    Source/Renderer/Culling/GPUCullingPass.cpp Source/Renderer/Culling/GPUInstanceCullingPass.cpp Source/Renderer/Culling/MeshletCulling.cpp Source/Renderer/Culling/IndexedIndirectDrawCount.cpp Source/Renderer/Culling/HiZBuffer.cpp Source/Renderer/Culling/HiZPass.cpp Source/Renderer/GPUSceneDatabase.cpp
    Source/Renderer/Vulkan/buffer.cpp Source/Renderer/Vulkan/upload_context.cpp Source/Renderer/Vulkan/depth_buffer.cpp Source/Renderer/Vulkan/graphics_pipeline.cpp Source/Renderer/Vulkan/hdr_buffer.cpp Source/Renderer/Vulkan/msaa.cpp Source/Renderer/Vulkan/renderer.cpp Source/Renderer/Vulkan/renderer_scene_helpers.cpp Source/Renderer/Vulkan/CameraController.cpp Source/Renderer/Vulkan/ViewportRenderTarget.cpp Source/Renderer/Vulkan/GpuTimestampProfiler.cpp Source/Renderer/Vulkan/MemoryBudgetManager.cpp Source/Renderer/Vulkan/shadow_map.cpp Source/Renderer/Vulkan/swapchain.cpp Source/Renderer/Vulkan/vulkan_device.cpp Source/Renderer/Vulkan/vma_impl.cpp
    Source/Renderer/Particles/ParticleSystem.cpp Source/Renderer/Geometry/Terrain.cpp Source/Renderer/Geometry/ProceduralCloud.cpp Source/Renderer/Geometry/Meshlet.cpp
    ${PROJECT_SOURCE_DIR}/Platform/SDL/SDLInput.cpp)
file(GLOB_RECURSE ENGINE_PUBLIC_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Include/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/Include/*.hpp")
target_sources(Engine PRIVATE FILE_SET engine_public_headers TYPE HEADERS BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/Include" FILES ${ENGINE_PUBLIC_HEADERS})
target_link_libraries(Engine PUBLIC SDL3::SDL3 Vulkan::Vulkan PRIVATE DearImGui physx_lib CMP_Core GameEngineTinyExr)
target_compile_definitions(Engine PRIVATE PX_PHYSX_STATIC_LIB
    GAMEENGINE_SLANGC_PATH="${GAMEENGINE_SLANGC}")
target_include_directories(Engine PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include> ${GLM_INCLUDE_DIR} ${VMA_INCLUDE_DIR} PRIVATE "${PROJECT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/Source" "${PROJECT_SOURCE_DIR}/ThridParty/stb")
gameengine_enable_warnings(Engine)
gameengine_enable_sanitizers(Engine)
if(WIN32)
    set_target_properties(Engine PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()
