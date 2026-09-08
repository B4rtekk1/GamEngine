add_executable(Editor
    Source/main.cpp Source/App/EditorEntityHelpers.cpp Source/ComponentDescriptor.cpp Source/EditorUi.cpp Source/UI/EditorTheme.cpp Source/UI/EditorWidgets.cpp Source/UI/PropertyGrid.cpp Source/UI/ComponentCard.cpp Source/EditorPreferences.cpp
    Source/Panels/EditorStyle.cpp Source/Panels/EditorSceneSession.cpp Source/Panels/HierarchyPanel.cpp Source/Panels/ComponentsPanel.cpp Source/Panels/AssetManagerPanel.cpp Source/AssetImporter.cpp Source/Panels/ConsolePanel.cpp Source/Panels/TerminalPanel.cpp ${PROJECT_SOURCE_DIR}/Platform/Terminal/TerminalBuffer.cpp)
if(WIN32)
    target_sources(Editor PRIVATE ${PROJECT_SOURCE_DIR}/Platform/Windows/Win32FileWatcher.cpp ${PROJECT_SOURCE_DIR}/Platform/Windows/ConPtyTerminal.cpp)
elseif(UNIX AND NOT APPLE)
    target_sources(Editor PRIVATE ${PROJECT_SOURCE_DIR}/Platform/Linux/InotifyFileWatcher.cpp)
endif()
target_include_directories(Editor PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Include" "${PROJECT_SOURCE_DIR}")
target_link_libraries(Editor PRIVATE DearImGui Engine SDL3::SDL3 Vulkan::Vulkan)
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
