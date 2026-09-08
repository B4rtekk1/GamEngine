option(GAMEENGINE_INSTALL_PORTABLE_EDITOR "Install a self-contained GamEngine Editor package" ON)
if(GAMEENGINE_INSTALL_PORTABLE_EDITOR)
    install(TARGETS Editor Engine DearImGui COMPONENT GamEngineEditor RUNTIME DESTINATION . LIBRARY DESTINATION . ARCHIVE DESTINATION lib)
    install(IMPORTED_RUNTIME_ARTIFACTS SDL3::SDL3-shared COMPONENT GamEngineEditor RUNTIME DESTINATION . LIBRARY DESTINATION .)
    install(DIRECTORY "${GAMEENGINE_SHADER_OUTPUT_DIR}/" DESTINATION shaders COMPONENT GamEngineEditor)
    add_custom_target(PackageEditor
        COMMAND ${CMAKE_COMMAND} --install "${CMAKE_BINARY_DIR}" --config $<CONFIG> --component GamEngineEditor --prefix "${CMAKE_BINARY_DIR}/GamEngineEditor"
        DEPENDS Editor EngineShaders COMMENT "Creating portable GamEngine Editor package")
endif()
