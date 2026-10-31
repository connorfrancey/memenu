cmake_minimum_required(VERSION 4.2)
project(medge_menu LANGUAGES C CXX)
set(MEMENU_VERSION "0.1.0")   # Shown by the installer, stamped into the loader

# -------------------------------------------------------------------------------------------------
# Platform Guards (Mirror's Edge is a 32-bit Windows process)
if(NOT WIN32)
  message(FATAL_ERROR "${PROJECT_NAME} is Windows-only.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Mirror's Edge is 32-bit. Use an x86 toolchain/preset (cmake -A Win32 / x86-debug).")
endif()

# -------------------------------------------------------------------------------------------------
# Global Settings (must come before dependencies so they inherit them)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # static CRT

# Optional targets: by default only medge_menu.dll is built
option(MEMENU_BUILD_TESTS     "Build the GoogleTest unit tests (memenu_tests)" OFF)
option(MEMENU_BUILD_INSTALLER "Build the dinput8.dll loader and memenu_installer.exe" OFF)
option(MEMENU_BUILD_LAUNCHER  "Build medge_launcher.exe (injects the DLL into a running game)" OFF)
option(MEMENU_BUILD_TESTHOST  "Build memenu_testhost.exe (loads the DLL into a D3D9 test app: memenu verbose-build)" OFF)
# Dev tooling packages (Ccache, CPMLicenses, Format.cmake): not needed to build the DLL
option(MEMENU_DEV_TOOLS       "Fetch Ccache.cmake, CPMLicenses.cmake and Format.cmake" OFF)
# Performance optimizations (src/core/optimizations.h): caches, cached object/name lookups, the scheduler's intervals,
# skipping and budget. Optional: OFF (`memenu build --no-optimizations`) does the work every time, nothing else changes.
option(MEMENU_OPTIMIZATIONS   "Build the performance optimizations (caches, scheduler intervals/budget)" ON)
# Staged build (`memenu minimal --stage N`, presets x86-minimal-*): stage 0 is only the D3D9 hook and ImGui's demo
# window (links only MinHook, ImGui and d3d9); each stage adds back a layer or a group of features, least likely to
# break the game first, up to the whole trainer. The stages are listed in cmake/Features.cmake.
option(MEMENU_MINIMAL         "Staged build: only what MEMENU_MINIMAL_STAGE adds back (ignores the feature options)" OFF)
set(MEMENU_MINIMAL_STAGE 0 CACHE STRING "Staged build: how far to go (0 = D3D9 hook + ImGui demo; see cmake/Features.cmake)")

# CMake 4 dropped support for cmake_minimum_required < 3.5; this lets older dependencies configure
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Use CPM.cmake
include(cmake/CPM.cmake)

# -------------------------------------------------------------------------------------------------
# Features: one option per module (MEMENU_FEATURE_<NAME>), per category and a master switch, or the staged build's
# stage (cmake/Features.cmake). Sets MEMENU_FEATURE_SOURCES, MEMENU_GAME, MEMENU_BARE and MEMENU_STAGE.
include(cmake/Features.cmake)

# -------------------------------------------------------------------------------------------------
# Find sources and headers
file(GLOB_RECURSE SOURCE_FILES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/src/*.c
  ${CMAKE_SOURCE_DIR}/src/*.cpp)
file(GLOB_RECURSE HEADER_FILES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/src/*.h
  ${CMAKE_SOURCE_DIR}/src/*.hpp)

# Feature modules (src/features/<category>/), the TAS engine and the game layer come from cmake/Features.cmake.
# Only the always-built files stay from the glob: core, menu, hooks, config, themes and features/tabs.cpp.
set(_feature_files ${SOURCE_FILES})
list(FILTER _feature_files INCLUDE REGEX "/src/features/[^/]+/")
list(FILTER SOURCE_FILES EXCLUDE REGEX "/src/(features/[^/]+|tas|game|render)/")
foreach(file IN LISTS _feature_files)   # A new module file has to be listed there, or it would silently not build
  if(NOT file IN_LIST MEMENU_ALL_FEATURE_SOURCES)
    message(FATAL_ERROR "${file} is not listed in cmake/Features.cmake: add a memenu_feature() for it")
  endif()
endforeach()
list(APPEND SOURCE_FILES ${MEMENU_FEATURE_SOURCES})
if(NOT MEMENU_GAME)   # Keep the IDE tree to what is built
  list(FILTER HEADER_FILES EXCLUDE REGEX "/src/(tas|game)/")
endif()
if(MEMENU_BARE)    # Staged build before the menu: hooks.cpp + src/minimal/ (its own DllMain and gui::)
  set(SOURCE_FILES ${PROJECT_SOURCE_DIR}/src/hooks.cpp ${PROJECT_SOURCE_DIR}/src/minimal/minimal.cpp)
  set(HEADER_FILES ${PROJECT_SOURCE_DIR}/src/hooks.h ${PROJECT_SOURCE_DIR}/src/gui.h ${PROJECT_SOURCE_DIR}/src/paths.h)
  if(MEMENU_STAGE GREATER_EQUAL MEMENU_STAGE_LOGGING)
    list(APPEND SOURCE_FILES ${PROJECT_SOURCE_DIR}/src/log.cpp)
    list(APPEND HEADER_FILES ${PROJECT_SOURCE_DIR}/src/log.h)
  endif()
  if(MEMENU_STAGE GREATER_EQUAL MEMENU_STAGE_THEMES)
    list(APPEND SOURCE_FILES ${PROJECT_SOURCE_DIR}/src/theme.cpp ${PROJECT_SOURCE_DIR}/src/theme_backend.cpp)
    list(APPEND HEADER_FILES ${PROJECT_SOURCE_DIR}/src/theme.h ${PROJECT_SOURCE_DIR}/src/theme_backend.h)
  endif()
else()
  list(FILTER SOURCE_FILES EXCLUDE REGEX "/src/minimal/")
endif()

# Injected mod => DLL, not an executable
add_library(${PROJECT_NAME} SHARED ${HEADER_FILES} ${SOURCE_FILES})
set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")

# build_info.cpp: a new build identifier (UUID, commit, build time) on every build, logged at startup
include(cmake/BuildInfo.cmake)
memenu_add_build_info(${PROJECT_NAME})

source_group(TREE ${PROJECT_SOURCE_DIR}/src FILES ${HEADER_FILES} ${SOURCE_FILES})

target_include_directories(${PROJECT_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(${PROJECT_NAME} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
  MEMENU_GAME=$<BOOL:${MEMENU_GAME}>
  MEMENU_OPTIMIZATIONS=$<BOOL:${MEMENU_OPTIMIZATIONS}>
  MEMENU_MINIMAL=$<BOOL:${MEMENU_BARE}>     # The code's "minimal": no menu, src/minimal/ instead (stages 0-2)
  MEMENU_MINIMAL_STAGE=$<IF:$<BOOL:${MEMENU_BARE}>,${MEMENU_STAGE},0>)
foreach(feature IN LISTS MEMENU_ENABLED_FEATURES)    # MEMENU_FEATURE_<NAME>=0/1 in the code
  string(TOUPPER "${feature}" _name)
  target_compile_definitions(${PROJECT_NAME} PRIVATE MEMENU_FEATURE_${_name}=1)
endforeach()
foreach(feature IN LISTS MEMENU_DISABLED_FEATURES)
  string(TOUPPER "${feature}" _name)
  target_compile_definitions(${PROJECT_NAME} PRIVATE MEMENU_FEATURE_${_name}=0)
endforeach()

if(MSVC)
target_compile_options(${PROJECT_NAME} PRIVATE /W4 /permissive- /utf-8)
else()
target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)
target_link_options(${PROJECT_NAME} PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# Copy the finished DLL to <repo>/build/ so it's in one fixed place regardless of preset/config
option(MEMENU_COPY_TO_BUILD_DIR "Copy medge_menu.dll to <repo>/build/ after each build" ON)
if(MEMENU_MINIMAL)   # Don't overwrite the full build's DLL in build/
  set(_memenu_output_default "${PROJECT_SOURCE_DIR}/build/minimal")
else()
  set(_memenu_output_default "${PROJECT_SOURCE_DIR}/build")
endif()
set(MEMENU_BUILD_OUTPUT_DIR "${_memenu_output_default}" CACHE PATH "Where MEMENU_COPY_TO_BUILD_DIR copies the DLL")
if(MEMENU_COPY_TO_BUILD_DIR)
  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MEMENU_BUILD_OUTPUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${PROJECT_NAME}> "${MEMENU_BUILD_OUTPUT_DIR}"
    COMMENT "Copying ${PROJECT_NAME}.dll to ${MEMENU_BUILD_OUTPUT_DIR}")
endif()

# -------------------------------------------------------------------------------------------------
# Libraries

include(cmake/Libraries.cmake)

# -------------------------------------------------------------------------------------------------
# Loader (dinput8.dll proxy next to the game exe) and its installer: the normal way to use the menu
if(MEMENU_BUILD_INSTALLER)
  add_subdirectory(loader)
  add_subdirectory(installer)
endif()

# Launcher (injects the DLL into a running game without installing anything)
if(MEMENU_BUILD_LAUNCHER)
  add_subdirectory(launcher)
endif()

# Test host (loads the DLL into a small D3D9 app instead of the game, and drives it: `memenu verbose-build`)
if(MEMENU_BUILD_TESTHOST)
  add_subdirectory(tools/testhost)
endif()

# -------------------------------------------------------------------------------------------------
# Tests (GoogleTest)
if(MEMENU_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
