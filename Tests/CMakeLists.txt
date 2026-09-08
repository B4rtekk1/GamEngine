include(FetchContent)
FetchContent_Declare(googletest GIT_REPOSITORY https://github.com/google/googletest.git GIT_TAG v1.17.0 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(googletest)

# These unit tests deliberately compile the small, dependency-free slice of
# Engine they exercise.  Splitting Engine into internal libraries is the next
# step; until then this avoids linking the renderer and its runtime setup.
add_executable(GamEngineTests
    MathTests.cpp EcsTests.cpp GeneralTests.cpp ComponentTests.cpp UiAndLayoutTests.cpp TimeTests.cpp SceneBuilderTests.cpp ScriptRegistryTests.cpp ProjectTests.cpp GPUSceneDatabaseTests.cpp
    ${PROJECT_SOURCE_DIR}/Engine/Source/Core/Camera.cpp ${PROJECT_SOURCE_DIR}/Engine/Source/Core/Time.cpp ${PROJECT_SOURCE_DIR}/Engine/Source/Project.cpp
    ${PROJECT_SOURCE_DIR}/Engine/Source/Renderer/Geometry/Terrain.cpp ${PROJECT_SOURCE_DIR}/Engine/Source/Renderer/Geometry/ProceduralCloud.cpp ${PROJECT_SOURCE_DIR}/Engine/Source/Renderer/GPUSceneDatabase.cpp
    ${PROJECT_SOURCE_DIR}/Engine/Source/UI/Canvas.cpp ${PROJECT_SOURCE_DIR}/Engine/Source/UI/RectTransform.cpp ${PROJECT_SOURCE_DIR}/Engine/Source/UI/UIBatch.cpp ${PROJECT_SOURCE_DIR}/Engine/Source/UI/Vulkan/UIFontAtlas.cpp ${PROJECT_SOURCE_DIR}/Engine/Source/UI/Vulkan/UIFontAtlasRuntime.cpp ${PROJECT_SOURCE_DIR}/Engine/Source/Scripting/ScriptRegistry.cpp)
target_link_libraries(GamEngineTests PRIVATE GTest::gtest_main)
target_include_directories(GamEngineTests PRIVATE "${PROJECT_SOURCE_DIR}/Engine/Include" ${GLM_INCLUDE_DIR})
gameengine_enable_warnings(GamEngineTests)
gameengine_enable_sanitizers(GamEngineTests)

add_executable(PhysicsMeshTests PhysicsMeshTests.cpp PhysXPhysicsTests.cpp ScriptSystemTests.cpp)
target_link_libraries(PhysicsMeshTests PRIVATE Engine GTest::gtest_main)
gameengine_enable_warnings(PhysicsMeshTests)
gameengine_enable_sanitizers(PhysicsMeshTests)
gameengine_copy_runtime_dependencies(PhysicsMeshTests)

# Kept independent from the historical mixed component test target so the
# compiler contract remains runnable without a Vulkan or PhysX runtime.
add_executable(ShaderGraphCompilerTests
    ShaderGraphTests.cpp
    ${PROJECT_SOURCE_DIR}/Engine/Source/Renderer/ShaderGraph/ShaderGraphCompiler.cpp
    ${PROJECT_SOURCE_DIR}/Engine/Source/Renderer/ShaderGraph/ShaderNodeFactory.cpp
    ${PROJECT_SOURCE_DIR}/Engine/Source/Renderer/ShaderGraph/ShaderNodeRegistry.cpp)
target_link_libraries(ShaderGraphCompilerTests PRIVATE GTest::gtest_main)
target_include_directories(ShaderGraphCompilerTests PRIVATE "${PROJECT_SOURCE_DIR}/Engine/Include" ${GLM_INCLUDE_DIR})
gameengine_enable_warnings(ShaderGraphCompilerTests)
gameengine_enable_sanitizers(ShaderGraphCompilerTests)
include(GoogleTest)
gtest_discover_tests(GamEngineTests)
gtest_discover_tests(PhysicsMeshTests)
gtest_discover_tests(ShaderGraphCompilerTests)
