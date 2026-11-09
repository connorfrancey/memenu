# Cross-compiles the 32-bit Windows DLL on Linux: clang-cl and lld-link (LLVM) against Microsoft's CRT and Windows SDK as
# xwin lays them out (tools/linux/setup.sh). Programs built for testing run under Wine (CMAKE_CROSSCOMPILING_EMULATOR),
# so ctest works as on Windows. Used by the linux-x86-* presets (CMakePresets.json).
#
# Where things are comes from the environment that tools/linux/setup.sh writes (env.sh): MEMENU_XWIN_DIR (crt/ and
# sdk/), MEMENU_LLVM_DIR and MEMENU_WINE_DIR (each with a bin/), or from PATH.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR X86)

set(MEMENU_XWIN_DIR "$ENV{MEMENU_XWIN_DIR}" CACHE PATH "Microsoft's CRT and Windows SDK, as xwin splat writes them")
if(NOT EXISTS "${MEMENU_XWIN_DIR}/crt/include" OR NOT EXISTS "${MEMENU_XWIN_DIR}/sdk/include/um")
  message(FATAL_ERROR "No CRT / Windows SDK at MEMENU_XWIN_DIR='${MEMENU_XWIN_DIR}'. Run tools/linux/setup.sh, then "
                      "source the env.sh it writes.")
endif()

set(_llvm_hints "$ENV{MEMENU_LLVM_DIR}/bin")
find_program(MEMENU_CLANG_CL NAMES clang-cl HINTS ${_llvm_hints} REQUIRED)
find_program(MEMENU_LLD_LINK NAMES lld-link HINTS ${_llvm_hints} REQUIRED)
find_program(MEMENU_LLVM_RC NAMES llvm-rc HINTS ${_llvm_hints} REQUIRED)
find_program(MEMENU_LLVM_MT NAMES llvm-mt HINTS ${_llvm_hints})
find_program(MEMENU_LLVM_LIB NAMES llvm-lib HINTS ${_llvm_hints} REQUIRED)

set(CMAKE_C_COMPILER "${MEMENU_CLANG_CL}")
set(CMAKE_CXX_COMPILER "${MEMENU_CLANG_CL}")
set(CMAKE_C_COMPILER_TARGET i686-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET i686-pc-windows-msvc)
set(CMAKE_LINKER "${MEMENU_LLD_LINK}")
set(CMAKE_AR "${MEMENU_LLVM_LIB}")
set(CMAKE_RC_COMPILER "${MEMENU_LLVM_RC}")
if(MEMENU_LLVM_MT)
  set(CMAKE_MT "${MEMENU_LLVM_MT}")
endif()

# Headers: CRT, then the SDK (ucrt, um, shared, winrt), as system headers (/imsvc: no warnings from them)
set(_x "${MEMENU_XWIN_DIR}")
set(_includes "/imsvc\"${_x}/crt/include\" /imsvc\"${_x}/sdk/include/ucrt\" /imsvc\"${_x}/sdk/include/um\" /imsvc\"${_x}/sdk/include/shared\" /imsvc\"${_x}/sdk/include/winrt\"")
set(CMAKE_C_FLAGS_INIT "${_includes}")
set(CMAKE_CXX_FLAGS_INIT "${_includes}")
set(CMAKE_RC_FLAGS_INIT "-I \"${_x}/crt/include\" -I \"${_x}/sdk/include/ucrt\" -I \"${_x}/sdk/include/um\" -I \"${_x}/sdk/include/shared\"")

# Libraries
set(_libpaths "/libpath:\"${_x}/crt/lib/x86\" /libpath:\"${_x}/sdk/lib/um/x86\" /libpath:\"${_x}/sdk/lib/ucrt/x86\"")
foreach(kind EXE SHARED MODULE)
  set(CMAKE_${kind}_LINKER_FLAGS_INIT "${_libpaths}")
endforeach()

# Test programs run under Wine (ctest, gtest_discover_tests)
find_program(MEMENU_WINE NAMES wine HINTS "$ENV{MEMENU_WINE_DIR}/bin")
if(MEMENU_WINE)
  set(CMAKE_CROSSCOMPILING_EMULATOR "${MEMENU_WINE}")
endif()

# Only the host's tools are programs; headers and libraries come from the paths above
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
