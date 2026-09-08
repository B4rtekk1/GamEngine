add_executable(Editor
    Source/main.cpp Source/App/EditorEntityHelpers.cpp Source/ComponentDescriptor.cpp Source/EditorUi.cpp Source/UI/EditorTheme.cpp Source/UI/EditorWidgets.cpp Source/UI/PropertyGrid.cpp Source/UI/ComponentCard.cpp Source/EditorPreferences.cpp
    Source/Panels/EditorStyle.cpp Source/Panels/EditorSceneSession.cpp Source/Panels/HierarchyPanel.cpp Source/Panels/ComponentsPanel.cpp Source/Panels/AssetManagerPanel.cpp Source/Panels/ShaderGraphPanel.cpp Source/AssetImporter.cpp Source/Panels/ConsolePanel.cpp Source/Panels/TerminalPanel.cpp ${PROJECT_SOURCE_DIR}/Platform/Terminal/TerminalBuffer.cpp)
if(WIN32)
    target_sources(Editor PRIVATE ${PROJECT_SOURCE_DIR}/Platform/Windows/Win32FileWatcher.cpp ${PROJECT_SOURCE_DIR}/Platform/Windows/ConPtyTerminal.cpp ${PROJECT_SOURCE_DIR}/Platform/Windows/WindowsUserPaths.cpp)
elseif(UNIX AND NOT APPLE)
    target_sources(Editor PRIVATE ${PROJECT_SOURCE_DIR}/Platform/Linux/InotifyFileWatcher.cpp ${PROJECT_SOURCE_DIR}/Platform/Linux/LinuxUserPaths.cpp)
endif()
target_include_directories(Editor PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Include" "${PROJECT_SOURCE_DIR}")
target_compile_definitions(Editor PRIVATE GAMEENGINE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(Editor PRIVATE DearImGui GameEngineImNodes Engine SDL3::SDL3 Vulkan::Vulkan)
if(WIN32)
    target_link_libraries(Editor PRIVATE Comdlg32 Shell32)
endif()
gameengine_enable_warnings(Editor)
gameengine_enable_sanitizers(Editor)
gameengine_copy_runtime_dependencies(Editor)
add_dependencies(Editor EngineShaders)
add_custom_command(TARGET Editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Editor>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${GAMEENGINE_SHADER_OUTPUT_DIR}" "$<TARGET_FILE_DIR:Editor>/shaders"
    COMMENT "Copying engine shaders for Editor")

# Keep development and portable Editors on the exact same project-script
# contract. Packaging.cmake installs this layout for a portable package; this
# post-build copy makes it available beside an Editor launched from its build
# directory as well.
add_custom_command(TARGET Editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Editor>/SDK/Lib"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Editor>/SDK/ThirdParty"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Editor>/SDK/Tools"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Editor>/SDK/GameScripts"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/Engine/Include" "$<TARGET_FILE_DIR:Editor>/SDK/Include"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${GLM_INCLUDE_DIR}/glm" "$<TARGET_FILE_DIR:Editor>/SDK/ThirdParty/glm"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_LINKER_FILE:Engine>" "$<TARGET_FILE_DIR:Editor>/SDK/Lib/Engine.lib"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/Player/GameScriptsPCH.h" "$<TARGET_FILE_DIR:Editor>/SDK/GameScriptsPCH.h"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/Tools/GenerateGameScriptsModule.cmake" "$<TARGET_FILE_DIR:Editor>/SDK/Tools/GenerateGameScriptsModule.cmake"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/cmake/GameScriptsStandalone/CMakeLists.txt" "$<TARGET_FILE_DIR:Editor>/SDK/GameScripts/CMakeLists.txt"
    COMMENT "Copying C++ scripting SDK for Editor")
