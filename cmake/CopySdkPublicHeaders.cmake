# Copies the C++ scripting SDK's public headers: the entry headers below and
# every engine header they include, directly or transitively. Renderer,
# Vulkan and other implementation headers stay out of the SDK; game scripts
# cannot compile them anyway (their Vulkan, SDL and PhysX headers are not
# shipped) and they are not an API scripts may depend on.
#
# Run as a script:
#   cmake -DGE_SDK_HEADER_SOURCE=<Engine/Include> -DGE_SDK_HEADER_DESTINATION=<SDK/Include>
#         -P CopySdkPublicHeaders.cmake
# or include() it from install(CODE) with the same variables set.
#
# The destination's Engine/ folder is replaced, so headers that left the
# public set do not linger in an existing SDK.

cmake_minimum_required(VERSION 3.25)

# What game scripts include (see the project templates and
# Player/GameScriptsPCH.h). A directory entry adds every header in it.
set(GE_SDK_PUBLIC_ENTRY_HEADERS
    Engine/Engine.h
    Engine/ECS/Actor.h
    Engine/Input/Input.h
    Engine/Physics/PhysicsSystem.h
    Engine/Scene/Scene.h
    Engine/Scripting
    Engine/Plugins
    Engine/Math)

if(NOT GE_SDK_HEADER_SOURCE OR NOT GE_SDK_HEADER_DESTINATION)
    message(FATAL_ERROR "GE_SDK_HEADER_SOURCE and GE_SDK_HEADER_DESTINATION are required")
endif()
file(TO_CMAKE_PATH "${GE_SDK_HEADER_SOURCE}" _source)
file(TO_CMAKE_PATH "${GE_SDK_HEADER_DESTINATION}" _destination)

set(_pending "")
foreach(_entry IN LISTS GE_SDK_PUBLIC_ENTRY_HEADERS)
    if(IS_DIRECTORY "${_source}/${_entry}")
        file(GLOB _directory_headers LIST_DIRECTORIES false RELATIVE "${_source}"
             "${_source}/${_entry}/*.h" "${_source}/${_entry}/*.hpp" "${_source}/${_entry}/*.inl")
        list(APPEND _pending ${_directory_headers})
    elseif(EXISTS "${_source}/${_entry}")
        list(APPEND _pending "${_entry}")
    else()
        message(FATAL_ERROR "SDK entry header does not exist: ${_source}/${_entry}")
    endif()
endforeach()

# Include closure. Engine headers include each other either from the include
# root ("Engine/...") or relative to their own folder ("Entity.h").
set(_public "")
while(_pending)
    list(POP_FRONT _pending _header)
    if(_header IN_LIST _public)
        continue()
    endif()
    list(APPEND _public "${_header}")
    file(STRINGS "${_source}/${_header}" _include_lines REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]")
    get_filename_component(_header_directory "${_header}" DIRECTORY)
    foreach(_line IN LISTS _include_lines)
        if(NOT _line MATCHES "^[ \t]*#[ \t]*include[ \t]*[<\"]([^>\"]+)[>\"]")
            continue()
        endif()
        set(_included "${CMAKE_MATCH_1}")
        cmake_path(SET _relative_candidate NORMALIZE "${_header_directory}/${_included}")
        cmake_path(SET _root_candidate NORMALIZE "${_included}")
        if(_header_directory AND EXISTS "${_source}/${_relative_candidate}"
           AND NOT IS_DIRECTORY "${_source}/${_relative_candidate}")
            list(APPEND _pending "${_relative_candidate}")
        elseif(EXISTS "${_source}/${_root_candidate}" AND NOT IS_DIRECTORY "${_source}/${_root_candidate}")
            list(APPEND _pending "${_root_candidate}")
        endif()
        # Anything else is a standard or third-party header (glm ships in
        # SDK/ThirdParty).
    endforeach()
endwhile()

file(REMOVE_RECURSE "${_destination}/Engine")
foreach(_header IN LISTS _public)
    get_filename_component(_target_directory "${_destination}/${_header}" DIRECTORY)
    file(MAKE_DIRECTORY "${_target_directory}")
    file(COPY_FILE "${_source}/${_header}" "${_destination}/${_header}" ONLY_IF_DIFFERENT)
endforeach()
list(LENGTH _public _count)
message(STATUS "SDK public headers: ${_count} -> ${_destination}")
