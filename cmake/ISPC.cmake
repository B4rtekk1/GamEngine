include(FetchContent)

function(gameengine_fetch_ispc output_variable)
    set(ispc_version "1.31.0")

    if(NOT CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$")
        message(FATAL_ERROR "Automatic ISPC download is configured only for x86-64 hosts")
    endif()

    if(WIN32)
        set(ispc_archive "ispc-v${ispc_version}-windows.zip")
        set(ispc_directory "ispc-v${ispc_version}-windows")
        set(ispc_hash "SHA256=9a18793800b91d5be7b851513672cd9a81a985a5a5dfec5611c2318e8ad4140a")
        set(ispc_executable "ispc.exe")
    elseif(UNIX AND NOT APPLE)
        set(ispc_archive "ispc-v${ispc_version}-linux.tar.gz")
        set(ispc_directory "ispc-v${ispc_version}-linux")
        set(ispc_hash "SHA256=d74089c835e10fd7e2c4b9225ced38b87d1fb53d35c7ceabd48cdf035da11b11")
        set(ispc_executable "ispc")
    else()
        message(FATAL_ERROR "Automatic ISPC download is not configured for this platform")
    endif()

    FetchContent_Declare(ISPCTool
        URL "https://github.com/ispc/ispc/releases/download/v${ispc_version}/${ispc_archive}"
        URL_HASH "${ispc_hash}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR cmake-not-used)
    FetchContent_MakeAvailable(ISPCTool)

    foreach(ispc_path IN ITEMS
            "${ispctool_SOURCE_DIR}/bin/${ispc_executable}"
            "${ispctool_SOURCE_DIR}/${ispc_directory}/bin/${ispc_executable}")
        if(EXISTS "${ispc_path}")
            set(${output_variable} "${ispc_path}" PARENT_SCOPE)
            message(STATUS "GamEngine ISPC: ${ispc_path}")
            return()
        endif()
    endforeach()

    message(FATAL_ERROR "Downloaded ISPC ${ispc_version}, but ${ispc_executable} was not found in ${ispctool_SOURCE_DIR}")
endfunction()
