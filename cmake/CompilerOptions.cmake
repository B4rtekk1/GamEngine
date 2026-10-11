if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(UNIX)
    # Static dependencies are linked into the shared Engine library.
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
    # Development binaries are also copied into the game runtime template.
    # Preserve lookup of colocated libraries after that copy and after export.
    set(CMAKE_BUILD_RPATH "$ORIGIN")
    set(CMAKE_INSTALL_RPATH "$ORIGIN")
endif()

function(gameengine_enable_release_ipo target)
    get_target_property(target_auto_exports ${target} WINDOWS_EXPORT_ALL_SYMBOLS)
    if(WIN32 AND target_auto_exports)
        message(STATUS "IPO disabled for ${target}: WINDOWS_EXPORT_ALL_SYMBOLS requires readable COFF objects")
        return()
    endif()
    set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endfunction()

option(GAMEENGINE_ENABLE_WARNINGS "Enable strict compiler warnings" ON)
option(GAMEENGINE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(GAMEENGINE_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer where supported" OFF)
function(gameengine_enable_warnings target)
    if(NOT GAMEENGINE_ENABLE_WARNINGS)
        return()
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if(GAMEENGINE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(GAMEENGINE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
function(gameengine_enable_sanitizers target)
    if(NOT GAMEENGINE_ENABLE_SANITIZERS)
        return()
    endif()
    get_target_property(target_type ${target} TYPE)
    if(MSVC)
        target_compile_options(${target} PRIVATE /fsanitize=address)
        if(NOT target_type STREQUAL "STATIC_LIBRARY")
            target_link_options(${target} PRIVATE /INFERASANLIBS)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        if(NOT target_type STREQUAL "STATIC_LIBRARY")
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        endif()
    endif()
endfunction()
function(gameengine_copy_runtime_dependencies target)
    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:${target}> $<TARGET_FILE_DIR:${target}>
            COMMAND_EXPAND_LISTS)
    endif()
endfunction()

function(gameengine_copy_default_font target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/Fonts"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dearimgui_SOURCE_DIR}/misc/fonts/DroidSans.ttf" "$<TARGET_FILE_DIR:${target}>/Fonts/DroidSans.ttf"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/Licenses"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/Assets/Fonts/NOTICE.txt" "$<TARGET_FILE_DIR:${target}>/Licenses/NOTICE-Fonts.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/LICENSE" "$<TARGET_FILE_DIR:${target}>/Licenses/LICENSE-Fonts-Apache-2.0.txt"
        VERBATIM)
endfunction()
