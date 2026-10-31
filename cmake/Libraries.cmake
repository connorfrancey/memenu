# Libraries

# -------------------------------------------------------------------------------------------------

# MinHook (built by hand so we skip its own CMake project entirely)
CPMAddPackage(
        NAME minhook
        GITHUB_REPOSITORY TsudaKageyu/minhook
        GIT_TAG v1.3.4
        DOWNLOAD_ONLY YES
)
if(minhook_ADDED)
    add_library(minhook STATIC
            ${minhook_SOURCE_DIR}/src/buffer.c
            ${minhook_SOURCE_DIR}/src/hook.c
            ${minhook_SOURCE_DIR}/src/trampoline.c
            ${minhook_SOURCE_DIR}/src/hde/hde32.c   # 32-bit only, matches the guard in CMakeLists.txt
    )
    target_include_directories(minhook SYSTEM PUBLIC ${minhook_SOURCE_DIR}/include)
endif()

# ImGui
CPMAddPackage(
        NAME imgui
        GITHUB_REPOSITORY ocornut/imgui
        GIT_TAG v1.92.9b
        DOWNLOAD_ONLY YES
)
if(imgui_ADDED)
    add_library(imgui STATIC
            ${imgui_SOURCE_DIR}/imgui.cpp
            ${imgui_SOURCE_DIR}/imgui_draw.cpp
            ${imgui_SOURCE_DIR}/imgui_tables.cpp
            ${imgui_SOURCE_DIR}/imgui_widgets.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    )
    target_include_directories(imgui SYSTEM PUBLIC
            ${imgui_SOURCE_DIR}
            ${imgui_SOURCE_DIR}/backends
    )
    # dwmapi: imgui_impl_win32 links it with #pragma comment(lib), which MinGW ignores
    target_link_libraries(imgui PUBLIC d3d9 dwmapi)
endif()

# spdlog
CPMAddPackage(
        NAME spdlog
        GITHUB_REPOSITORY gabime/spdlog
        GIT_TAG v1.17.0
        OPTIONS "SPDLOG_INSTALL OFF"
)

# nlohmann/json: config file + theme parsing (release tarball is much smaller than cloning the repo)
CPMAddPackage(
        NAME nlohmann_json
        VERSION 3.12.0
        URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
        OPTIONS "JSON_Install OFF"
)

# imjson: ImGui themes from JSON (https://github.com/moleium/imjson)
# The repo has no library target we can use, so build its sources ourselves.
# Pin GIT_TAG to a commit hash once you have a version that works for you.
CPMAddPackage(
        NAME imjson
        GITHUB_REPOSITORY moleium/imjson
        GIT_TAG master
        DOWNLOAD_ONLY YES
)
if(imjson_ADDED)
    file(GLOB_RECURSE imjson_SOURCES CONFIGURE_DEPENDS
            ${imjson_SOURCE_DIR}/include/*.cpp
            ${imjson_SOURCE_DIR}/src/*.cpp)
    if(NOT imjson_SOURCES)
        message(FATAL_ERROR "imjson: no .cpp found under ${imjson_SOURCE_DIR}/include or /src - "
                "check where imjson.cpp lives in the repo and adjust the glob in cmake/Libraries.cmake")
    endif()
    add_library(imjson STATIC ${imjson_SOURCES})
    # imjson includes <imgui/imgui.h>, but the imgui target exposes its root (<imgui.h>): forward it
    file(WRITE ${CMAKE_BINARY_DIR}/imjson_compat/imgui/imgui.h "#pragma once\n#include \"${imgui_SOURCE_DIR}/imgui.h\"\n")
    target_include_directories(imjson PUBLIC ${imjson_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}/imjson_compat)
    target_link_libraries(imjson PUBLIC imgui nlohmann_json::nlohmann_json)
endif()

# Discord RPC
#include(${CMAKE_SOURCE_DIR}/cmake/discord-rpc.cmake)

# Portable File Dialogs
#include(${CMAKE_SOURCE_DIR}/cmake/portable-file-dialogs.cmake)

# Direct3D 9 comes from the Windows SDK (d3d9.h / d3d9.lib). Nothing uses D3DX, so the legacy DirectX SDK
# (June 2010) is not needed, and players don't need the d3dx9_43.dll runtime either.

# mmultiplayer

# Mirror's Edge 1.0 SDK from the connorfrancey/mmultiplayer fork (Client/sdk): generated Unreal Engine 3
# classes/structs for the game, as the CMake target me_sdk (alias mirrorsedge::sdk, header <ME_SDK.hpp>).
# It builds with MSVC and MinGW (cmake/mingw-i686.cmake) and exposes only the sdk/ folder, as SYSTEM
# includes (no warnings from it). Only Client/sdk is added, not the fork's own client DLL and launcher.
# To build against a local checkout instead: -DCPM_mmultiplayer_SOURCE=/path/to/mmultiplayer
# GIT_TAG is pinned to a main commit (merge of sdk-cmake) for reproducible builds.
# The me_sdk_docs target needs Doxygen at configure time: CI (and anyone without Doxygen) turns it off.
option(MEMENU_SDK_DOCS "Add the SDK documentation target (needs Doxygen)" ON)
CPMAddPackage(
        NAME mmultiplayer
        GITHUB_REPOSITORY connorfrancey/mmultiplayer
        GIT_TAG 7b1eb25ddc0fc1ce498d024e7b2f9d1980b9eca3
        SOURCE_SUBDIR Client/sdk
        OPTIONS
            "ME_SDK_BUILD_DOCS ${MEMENU_SDK_DOCS}"
)

# Catch2
CPMAddPackage(
    NAME catch2
    GITHUB_REPOSITORY catchorg/Catch2
    GIT_TAG v3.16.1
)

# -------------------------------------------------------------------------------------------------

target_link_libraries(${PROJECT_NAME} PRIVATE
        imgui imjson minhook d3d9 shell32 spdlog::spdlog nlohmann_json::nlohmann_json winmm me_sdk)

# -------------------------------------------------------------------------------------------------
