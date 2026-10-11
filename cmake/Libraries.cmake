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
    target_link_libraries(imgui PUBLIC d3d9)
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
    target_include_directories(imjson PUBLIC ${imjson_SOURCE_DIR}/include)
    target_link_libraries(imjson PUBLIC imgui nlohmann_json::nlohmann_json)
endif()

# Discord RPC
#include(${CMAKE_SOURCE_DIR}/cmake/discord-rpc.cmake)

# Portable File Dialogs
#include(${CMAKE_SOURCE_DIR}/cmake/portable-file-dialogs.cmake)

set(DIRECTX_SDK_ROOT "C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)")
target_include_directories(${PROJECT_NAME} PRIVATE
        "${DIRECTX_SDK_ROOT}/Include"
)

target_link_directories(${PROJECT_NAME} PRIVATE
        "${DIRECTX_SDK_ROOT}/Lib/x86"
)

# mmultiplayer

# mmultiplayer's Mirror's Edge 1.0 SDK (softsoundd/mmultiplayer, Client/sdk): generated Unreal Engine 3
# classes/structs for the game. It is MSVC-only (it uses __thiscall in templates).
# The repo has no license file: keep it in third_party/ (git-ignored) or let CPM download it, never commit it.
if(NOT MSVC)
    message(FATAL_ERROR "The mmultiplayer SDK only builds with MSVC. Use the x86-debug / x86-release presets.")
endif()

set(MMP_DIR "${CMAKE_SOURCE_DIR}/third_party/mmultiplayer" CACHE PATH
        "Extracted softsoundd/mmultiplayer repo (the folder that contains Client/). Downloaded with CPM if missing.")
if(NOT EXISTS "${MMP_DIR}/Client/sdk/ME_Basic.cpp")
    message(STATUS "mmultiplayer SDK not found in ${MMP_DIR}, downloading it with CPM")
    CPMAddPackage(NAME mmultiplayer GITHUB_REPOSITORY softsoundd/mmultiplayer GIT_TAG 2.5.2 DOWNLOAD_ONLY YES)
    set(MMP_DIR "${mmultiplayer_SOURCE_DIR}")
endif()

file(GLOB MMP_SDK_SOURCES CONFIGURE_DEPENDS ${MMP_DIR}/Client/sdk/*.cpp)
add_library(me_sdk STATIC ${MMP_SDK_SOURCES})
target_compile_options(me_sdk PRIVATE /w /bigobj /MP)

# Expose only one forwarding header instead of the whole Client/ folder (it also contains an old bundled
# ImGui, hook.h, menu.h, ... that must not shadow ours). Warnings from the SDK headers are silenced.
file(WRITE ${CMAKE_BINARY_DIR}/me_sdk_include/mirrorsedge_sdk.h
        "#pragma once\n#pragma warning(push, 0)\n#include \"${MMP_DIR}/Client/sdk.h\"\n#pragma warning(pop)\n")
target_include_directories(me_sdk INTERFACE ${CMAKE_BINARY_DIR}/me_sdk_include)



# -------------------------------------------------------------------------------------------------

target_link_libraries(${PROJECT_NAME} PRIVATE
        imgui imjson minhook d3d9 shell32 spdlog::spdlog nlohmann_json::nlohmann_json d3dx9 me_sdk)

# -------------------------------------------------------------------------------------------------
