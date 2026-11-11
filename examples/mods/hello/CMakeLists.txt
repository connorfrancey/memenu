# Hello, a native Mirage mod: its own CMake project, built against the mod API headers only (MIRAGE_API_DIR), so any
# compiler that targets 32-bit Windows works (MSVC, clang-cl). The built mod (mod.toml + hello.dll) goes to
# <build>/mod/hello: copy that folder into the menu's mods folder (Binaries\memenu\mods).
#
#   cmake -S examples/mods/hello -B build/hello -G Ninja -DCMAKE_BUILD_TYPE=Release   (from an x86 environment)
#   cmake --build build/hello
cmake_minimum_required(VERSION 3.20)
project(hello_mod LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Mirror's Edge is 32-bit: build the mod for x86 (an x86 environment, or clang-cl -m32)")
endif()

set(MIRAGE_API_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../api" CACHE PATH "Folder holding mirage/api.h")

add_library(hello SHARED hello.cpp)
target_include_directories(hello PRIVATE ${MIRAGE_API_DIR})
target_compile_features(hello PRIVATE cxx_std_17)
set_target_properties(hello PROPERTIES
        PREFIX ""
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"   # Static CRT: nothing to install next to it
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/mod/hello
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/mod/hello)
if(MSVC)
  target_compile_options(hello PRIVATE /W4 /utf-8)
endif()
add_custom_command(TARGET hello POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/mod.toml ${CMAKE_BINARY_DIR}/mod/hello/mod.toml)
