# Hook (native), a self-test mod (tests/mods): its own CMake project like a mod author's, built by `memenu game-test`. The
# built mod (mod.toml + hook-native.dll) goes to <build>/mod/hook-native.
cmake_minimum_required(VERSION 3.20)
project(hook_native_mod LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Mirror's Edge is 32-bit: build the mod for x86 (an x86 environment, or clang-cl -m32)")
endif()

set(MIRAGE_API_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../api" CACHE PATH "Folder holding mirage/api.h")

add_library(hook-native SHARED hook-native.cpp)
target_include_directories(hook-native PRIVATE ${MIRAGE_API_DIR})
target_compile_features(hook-native PRIVATE cxx_std_17)
set_target_properties(hook-native PROPERTIES
        PREFIX ""
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/mod/hook-native
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/mod/hook-native)
# mod.toml next to the DLL, on every build (it can change without the DLL being relinked)
add_custom_target(hook-native_manifest ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/mod.toml ${CMAKE_BINARY_DIR}/mod/hook-native/mod.toml
        DEPENDS hook-native)
