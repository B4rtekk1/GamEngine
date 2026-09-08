option(GAMEENGINE_INSTALL_PORTABLE_EDITOR "Install a self-contained GamEngine Editor package" ON)
if(GAMEENGINE_INSTALL_PORTABLE_EDITOR)
    install(TARGETS Editor Engine DearImGui COMPONENT GamEngineEditor RUNTIME DESTINATION . LIBRARY DESTINATION . ARCHIVE DESTINATION lib)
    # A project-side C++ script module links to this import library, not to a
    # GamEngine source/build tree.
    install(FILES "$<TARGET_LINKER_FILE:Engine>" DESTINATION SDK/Lib COMPONENT GamEngineEditor)
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/Engine/Include/" DESTINATION SDK/Include COMPONENT GamEngineEditor)
    # GLM_INCLUDE_DIR already contains the glm/ directory.  Preserve that
    # layout so <glm/...> resolves from SDK/ThirdParty.
    install(DIRECTORY "${GLM_INCLUDE_DIR}/glm" DESTINATION SDK/ThirdParty COMPONENT GamEngineEditor)
    install(FILES "${PROJECT_SOURCE_DIR}/Player/GameScriptsPCH.h" DESTINATION SDK COMPONENT GamEngineEditor)
    install(FILES "${PROJECT_SOURCE_DIR}/Tools/GenerateGameScriptsModule.cmake"
        DESTINATION SDK/Tools COMPONENT GamEngineEditor)
    install(FILES "${PROJECT_SOURCE_DIR}/cmake/GameScriptsStandalone/CMakeLists.txt"
        DESTINATION SDK/GameScripts COMPONENT GamEngineEditor)
    if(TARGET GameScripts)
        # Keep the script module built with the same configuration as the
        # Editor.  A Debug script DLL cannot safely exchange STL-owned data
        # with a Release Engine DLL (and vice versa).
        install(TARGETS GameScripts COMPONENT GamEngineEditor RUNTIME DESTINATION . LIBRARY DESTINATION . ARCHIVE DESTINATION lib)
    endif()
    install(IMPORTED_RUNTIME_ARTIFACTS SDL3::SDL3-shared COMPONENT GamEngineEditor RUNTIME DESTINATION . LIBRARY DESTINATION .)
    install(DIRECTORY "${GAMEENGINE_SHADER_OUTPUT_DIR}/" DESTINATION shaders COMPONENT GamEngineEditor)
    # Shader Graph authoring compiles a generated module that imports these
    # source files.  Cooked SPIR-V above is enough for Player, but not Editor.
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/Engine/Shaders/" DESTINATION ShaderSources COMPONENT GamEngineEditor)

    get_filename_component(GAMEENGINE_SLANG_RUNTIME_DIR "${GAMEENGINE_SLANGC}" DIRECTORY)
    set(GAMEENGINE_SLANG_RUNTIME_FILES "${GAMEENGINE_SLANGC}")
    if(EXISTS "${GAMEENGINE_SLANG_RUNTIME_DIR}/slang.slang")
        list(APPEND GAMEENGINE_SLANG_RUNTIME_FILES "${GAMEENGINE_SLANG_RUNTIME_DIR}/slang.slang")
    endif()
    if(WIN32)
        # slangc loads these Slang/SPIR-V backends from its own directory.  Do
        # not copy the whole Vulkan SDK (or its debug DLLs) into the Editor.
        file(GLOB GAMEENGINE_SLANG_RUNTIME_DLLS
            "${GAMEENGINE_SLANG_RUNTIME_DIR}/slang*.dll"
            "${GAMEENGINE_SLANG_RUNTIME_DIR}/glslang*.dll"
            "${GAMEENGINE_SLANG_RUNTIME_DIR}/SPIRV*.dll")
        list(FILTER GAMEENGINE_SLANG_RUNTIME_DLLS EXCLUDE REGEX "d\\.dll$")
        list(APPEND GAMEENGINE_SLANG_RUNTIME_FILES ${GAMEENGINE_SLANG_RUNTIME_DLLS})
    endif()
    install(FILES ${GAMEENGINE_SLANG_RUNTIME_FILES} DESTINATION Tools/Slang COMPONENT GamEngineEditor)
    # Recent Slang distributions keep their standard module beside slangc.
    # Keep it at the same relative location so Slang can find it at runtime.
    file(GLOB GAMEENGINE_SLANG_STANDARD_MODULE_DIRECTORIES LIST_DIRECTORIES TRUE
        "${GAMEENGINE_SLANG_RUNTIME_DIR}/slang-standard-module-*")
    foreach(GAMEENGINE_SLANG_STANDARD_MODULE_DIRECTORY IN LISTS GAMEENGINE_SLANG_STANDARD_MODULE_DIRECTORIES)
        if(IS_DIRECTORY "${GAMEENGINE_SLANG_STANDARD_MODULE_DIRECTORY}")
            install(DIRECTORY "${GAMEENGINE_SLANG_STANDARD_MODULE_DIRECTORY}" DESTINATION Tools/Slang
                COMPONENT GamEngineEditor)
        endif()
    endforeach()
    get_filename_component(GAMEENGINE_SLANG_SDK_DIR "${GAMEENGINE_SLANG_RUNTIME_DIR}" DIRECTORY)
    if(EXISTS "${GAMEENGINE_SLANG_SDK_DIR}/Licenses/LICENSE.txt")
        install(FILES "${GAMEENGINE_SLANG_SDK_DIR}/Licenses/LICENSE.txt"
            DESTINATION Tools/Slang COMPONENT GamEngineEditor RENAME LICENSE-VulkanSDK.txt)
    endif()

    add_custom_target(PackageEditor
        COMMAND ${CMAKE_COMMAND} --install "${CMAKE_BINARY_DIR}" --config $<CONFIG> --component GamEngineEditor --prefix "${CMAKE_BINARY_DIR}/GamEngineEditor"
        DEPENDS Editor EngineShaders COMMENT "Creating portable GamEngine Editor package")
endif()
