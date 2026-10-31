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

# CMake 4 dropped support for cmake_minimum_required < 3.5; this lets older dependencies configure
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Use CPM.cmake
include(cmake/CPM.cmake)

# -------------------------------------------------------------------------------------------------
# Find sources and headers
file(GLOB_RECURSE SOURCE_FILES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/src/*.c
  ${CMAKE_SOURCE_DIR}/src/*.cpp)
file(GLOB_RECURSE HEADER_FILES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/src/*.h
  ${CMAKE_SOURCE_DIR}/src/*.hpp)

# Injected mod => DLL, not an executable
add_library(${PROJECT_NAME} SHARED ${HEADER_FILES} ${SOURCE_FILES})
set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")

source_group(TREE ${PROJECT_SOURCE_DIR}/src FILES ${HEADER_FILES} ${SOURCE_FILES})

target_include_directories(${PROJECT_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(${PROJECT_NAME} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

if(MSVC)
target_compile_options(${PROJECT_NAME} PRIVATE /W4 /permissive- /utf-8)
else()
target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)
target_link_options(${PROJECT_NAME} PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# -------------------------------------------------------------------------------------------------
# Libraries

include(cmake/Libraries.cmake)

# -------------------------------------------------------------------------------------------------
# Loader (dinput8.dll proxy next to the game exe) and its installer: the normal way to use the menu
add_subdirectory(loader)
add_subdirectory(installer)

# Launcher (injects the DLL into a running game without installing anything)
add_subdirectory(launcher)
