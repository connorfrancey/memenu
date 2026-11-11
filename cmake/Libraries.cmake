include(${CMAKE_CURRENT_LIST_DIR}/Versions.cmake)   # Pinned versions (fmt, spdlog, UnrealLib, ...)

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

# Staged build, stages 0-2 (MEMENU_BARE): only what that stage adds is downloaded and linked
if(MEMENU_BARE)
    target_sources(imgui PRIVATE ${imgui_SOURCE_DIR}/imgui_demo.cpp)   # ImGui::ShowDemoWindow
    if(MEMENU_STAGE GREATER_EQUAL MEMENU_STAGE_LOGGING)
        set(_need_spdlog ON)
    endif()
    if(MEMENU_STAGE GREATER_EQUAL MEMENU_STAGE_THEMES)
        set(_need_themes ON)
    endif()
else()
    set(_need_spdlog ON)
    set(_need_themes ON)
endif()

# spdlog, on a separate fmt: UnrealLib links fmt::fmt too, and spdlog's bundled copy would define the same symbols
if(_need_spdlog)
CPMAddPackage(
        NAME fmt
        GITHUB_REPOSITORY fmtlib/fmt
        GIT_TAG ${FMT_VERSION}
        OPTIONS "FMT_INSTALL OFF" "FMT_TEST OFF" "FMT_DOC OFF" "FMT_MODULE OFF"
)
CPMAddPackage(
        NAME spdlog
        GITHUB_REPOSITORY gabime/spdlog
        GIT_TAG ${SPDLOG_VERSION}
        OPTIONS "SPDLOG_INSTALL OFF" "SPDLOG_FMT_EXTERNAL ON"
)

# zstd: compresses the previous session's log into logs/.old/ (log.cpp) and the TAS's memory snapshots
# (tas/snapshot.cpp). Built by hand like MinHook: zstd's own CMake project is large and sets its own runtime-library
# flags. The target keeps its old name; it has the decompressor too.
CPMAddPackage(
        NAME zstd
        GITHUB_REPOSITORY facebook/zstd
        GIT_TAG v1.5.7
        DOWNLOAD_ONLY YES
)
if(zstd_ADDED)
    file(GLOB zstd_SOURCES CONFIGURE_DEPENDS
            ${zstd_SOURCE_DIR}/lib/common/*.c
            ${zstd_SOURCE_DIR}/lib/compress/*.c
            ${zstd_SOURCE_DIR}/lib/decompress/*.c)
    add_library(zstd_compress STATIC ${zstd_SOURCES})
    target_include_directories(zstd_compress SYSTEM PUBLIC ${zstd_SOURCE_DIR}/lib)
    target_compile_definitions(zstd_compress PRIVATE ZSTD_DISABLE_ASM XXH_NAMESPACE=ZSTD_)
endif()
endif()

if(_need_themes)

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
endif()

if(MEMENU_BARE)
    target_link_libraries(${PROJECT_NAME} PRIVATE imgui minhook d3d9)
    if(_need_spdlog)
        target_link_libraries(${PROJECT_NAME} PRIVATE spdlog::spdlog zstd_compress)
    endif()
    if(_need_themes)
        target_link_libraries(${PROJECT_NAME} PRIVATE imjson nlohmann_json::nlohmann_json shell32)
    endif()
    return()
endif()

# imgui-filebrowser: the TAS module's Open / Save file browser, drawn with ImGui inside the menu (Windows' own dialog
# opened behind the fullscreen game and took the cursor). Header only; the repo has no tags, so a commit is pinned.
if("tas" IN_LIST MEMENU_ENABLED_FEATURES)
CPMAddPackage(
        NAME imgui_filebrowser
        GITHUB_REPOSITORY AirGuanZ/imgui-filebrowser
        GIT_TAG 47a1884524d6a3df5d99f70ecc82e1ed0ccd26cf
        DOWNLOAD_ONLY YES
)
if(imgui_filebrowser_ADDED)
    add_library(imgui_filebrowser INTERFACE)
    target_include_directories(imgui_filebrowser SYSTEM INTERFACE ${imgui_filebrowser_SOURCE_DIR})
    target_link_libraries(imgui_filebrowser INTERFACE imgui)
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE imgui_filebrowser)
endif()

# Tracy: profiler client, only with MEMENU_TRACY (src/core/profile.h). Set up for a DLL injected into the game:
#   TRACY_DELAYED_INIT, TRACY_MANUAL_LIFETIME  started and stopped by the menu (prof::Start/Stop), not by static
#                                               constructors: those run in DllMain under the loader lock, and the
#                                               profiler's threads must be gone before the DLL unloads
#   TRACY_ON_DEMAND                             nothing is collected until the profiler connects
#   TRACY_ONLY_LOCALHOST                        the game doesn't listen on the network
#   TRACY_NO_CRASH_HANDLER                      the menu's own crash logger (diag.cpp) stays the one in charge
#   TRACY_NO_SYSTEM_TRACING                     no ETW (needs admin, and traces the whole system)
#   TRACY_NO_SAMPLING                           no call stack sampling (needs admin, 64-bit only)
if(MEMENU_TRACY)
CPMAddPackage(
        NAME tracy
        GITHUB_REPOSITORY wolfpld/tracy
        GIT_TAG ${TRACY_VERSION}
        SYSTEM YES
        OPTIONS
            "TRACY_ENABLE ON"
            "TRACY_DELAYED_INIT ON"
            "TRACY_MANUAL_LIFETIME ON"
            "TRACY_ON_DEMAND ON"
            "TRACY_ONLY_LOCALHOST ON"
            "TRACY_NO_CRASH_HANDLER ON"
            "TRACY_NO_SYSTEM_TRACING ON"
            "TRACY_NO_SAMPLING ON"
            "TRACY_STATIC ON"
)
target_link_libraries(${PROJECT_NAME} PRIVATE Tracy::TracyClient)
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
option(MEMENU_SDK_DOCS "Add the SDK documentation target (needs Doxygen)" OFF)
# Only the game layer uses the SDK: a build without game features (MEMENU_GAME OFF) doesn't download it.
#
# Patches (cmake/patches/mmultiplayer-*.patch, applied by cmake/apply_patches.cmake: LF on every system, so Windows'
# CRLF checkout and Linux's LF one take the same patches):
#   mmultiplayer-fast-findobject.patch   UObject::FindObject compares the engine's name index before it builds full
#                                        names; every SDK function looks itself up on its first call, and building
#                                        the full name of all ~100k objects for that froze the game up to a second.
#   mmultiplayer-fast-names.patch        FNameEntry::GetName narrows ASCII with a loop instead of a std::locale per
#                                        call, and FName(const char*) compares names in place and remembers what it
#                                        found (it built a string for every cached and every table entry per lookup).
if(MEMENU_GAME)
find_package(Git REQUIRED)
CPMAddPackage(
        NAME mmultiplayer
        GITHUB_REPOSITORY connorfrancey/mmultiplayer
        GIT_TAG 7b1eb25ddc0fc1ce498d024e7b2f9d1980b9eca3
        SOURCE_SUBDIR Client/sdk
        PATCH_COMMAND "${CMAKE_COMMAND}" -DPATCH_DIR=${CMAKE_CURRENT_LIST_DIR}/patches -DPREFIX=mmultiplayer-
            -DWORK_DIR=<SOURCE_DIR> -DOUT_DIR=${CMAKE_BINARY_DIR}/patches -DGIT=${GIT_EXECUTABLE}
            -P "${CMAKE_CURRENT_LIST_DIR}/apply_patches.cmake"
        OPTIONS
            "ME_SDK_BUILD_DOCS ${MEMENU_SDK_DOCS}"
)
endif()

# UnrealLib: reads Unreal Engine 3 packages (.u/.upk/.me1), a C++ port of EliotVU/Unreal-Library. The Mirage runtime
# (src/runtime/) reads the game's script packages with it inside the game, so every build with the game layer links it;
# MEMENU_UNREALLIB adds its tests and status tool. Reuses the fmt/spdlog above and adds zlib.
if(MEMENU_UNREALLIB OR MEMENU_GAME OR MEMENU_BUILD_SDKGEN)
CPMAddPackage(
        NAME UnrealLib
        GITHUB_REPOSITORY connorfrancey/UnrealLib
        GIT_TAG ${UNREALLIB_GIT_TAG}
        OPTIONS
            "UNREALLIB_BUILD_TESTS OFF"
            "UNREALLIB_BUILD_TOOLS OFF"
            "UNREALLIB_BUILD_EXAMPLES OFF"
)
endif()

# toml11 (https://github.com/ToruNiina/toml11): TOML for Mirage's own files: mod manifests (mod.toml, step 4: the
# runtime will need it too) and the SDK generator's configuration (tools/sdkgen/sdkgen.toml). Header only.
if(MEMENU_BUILD_SDKGEN)
CPMAddPackage(
        NAME toml11
        GITHUB_REPOSITORY ToruNiina/toml11
        GIT_TAG v${TOML_VERSION}
        OPTIONS
            "TOML11_INSTALL OFF"
            "TOML11_BUILD_EXAMPLES OFF"
            "TOML11_BUILD_TESTS OFF"
)
endif()

# Lua 5.4: the language of Mirage mods (src/runtime/lua_vm.cpp). Built from the sources as a static library, compiled as
# C++: a Lua error is then a C++ exception, so it unwinds the C++ frames between the error and the pcall (destructors
# run) instead of longjmp-ing over them. Without the standalone interpreter/compiler and the amalgamation/test files.
if(MEMENU_GAME)
  CPMAddPackage(
      NAME lua
      GITHUB_REPOSITORY lua/lua
      GIT_TAG ${LUA_VERSION}
      DOWNLOAD_ONLY YES
  )
  if(lua_ADDED)
    file(GLOB _lua_sources CONFIGURE_DEPENDS ${lua_SOURCE_DIR}/*.c)
    list(FILTER _lua_sources EXCLUDE REGEX "/(lua|luac|onelua|ltests)[.]c$")
    set_source_files_properties(${_lua_sources} PROPERTIES LANGUAGE CXX)
    add_library(lua_static STATIC ${_lua_sources})
    target_include_directories(lua_static SYSTEM PUBLIC ${lua_SOURCE_DIR})
    if(MSVC)
      target_compile_options(lua_static PRIVATE /TP /W0)     # C sources as C++; Lua's own code, not ours to warn about
    endif()
    add_library(lua::lua ALIAS lua_static)
  endif()
endif()

# GoogleTest (only needed for the unit tests)
if(MEMENU_BUILD_TESTS)
  CPMAddPackage(
      NAME googletest
      GITHUB_REPOSITORY google/googletest
      GIT_TAG v1.17.0
      OPTIONS
          "INSTALL_GTEST OFF"
          "BUILD_GMOCK OFF"
          "gtest_force_shared_crt OFF"   # Static CRT, like the rest of the project (CMAKE_MSVC_RUNTIME_LIBRARY)
  )
endif()

# nanobench (only needed for the benchmarks): header only, the implementation is compiled in bench/bench_main.cpp
if(MEMENU_BUILD_BENCHMARKS)
  CPMAddPackage(
      NAME nanobench
      GITHUB_REPOSITORY martinus/nanobench
      GIT_TAG v4.3.11
      DOWNLOAD_ONLY YES
  )
  if(nanobench_ADDED)
    add_library(nanobench INTERFACE)
    target_include_directories(nanobench SYSTEM INTERFACE ${nanobench_SOURCE_DIR}/src/include)
  endif()
endif()

if(MEMENU_DEV_TOOLS)
# CPM Licenses
CPMAddPackage(
        NAME CPMLicenses.cmake
        GITHUB_REPOSITORY cpm-cmake/CPMLicenses.cmake
        VERSION 0.0.7
)

CPMAddPackage(
        NAME Format.cmake
        VERSION 1.8.2
        GITHUB_REPOSITORY TheLartians/Format.cmake
        OPTIONS
        # set to yes skip cmake formatting
        "FORMAT_SKIP_CMAKE NO"
        # set to yes skip clang formatting
        "FORMAT_SKIP_CLANG NO"
        # path to exclude (optional, supports regular expressions)
        "CMAKE_FORMAT_EXCLUDE cmake/CPM.cmake"
        # extra arguments for cmake_format (optional)
        "CMAKE_FORMAT_EXTRA_ARGS -c /path/to/cmake-format.{yaml,json,py}"
)
endif()
# -------------------------------------------------------------------------------------------------

target_link_libraries(${PROJECT_NAME} PRIVATE
        imgui imjson minhook d3d9 shell32 spdlog::spdlog zstd_compress nlohmann_json::nlohmann_json winmm)
if(MEMENU_GAME)
  target_link_libraries(${PROJECT_NAME} PRIVATE me_sdk UnrealLib::Linker lua::lua)
endif()

# -------------------------------------------------------------------------------------------------
