# MEMENU_CCACHE: compile C and C++ through ccache (option in CMakeLists.txt). Included before any target exists, since
# CMAKE_<LANG>_COMPILER_LAUNCHER and CMAKE_MSVC_DEBUG_INFORMATION_FORMAT only apply to targets created after them.

if(NOT MEMENU_CCACHE)
    return()
endif()

find_program(CCACHE_PROGRAM ccache DOC "ccache executable (MEMENU_CCACHE)")
if(NOT CCACHE_PROGRAM)
    message(WARNING "MEMENU_CCACHE is on but ccache was not found: building without it. Install ccache "
            "(scoop install ccache / winget install ccache.ccache) or set CCACHE_PROGRAM to its path.")
    return()
endif()

if(CMAKE_GENERATOR MATCHES "Visual Studio")
    # The VS generator ignores compiler launchers
    message(WARNING "MEMENU_CCACHE: the Visual Studio generator can't use ccache; use Ninja (the presets do).")
    return()
endif()

set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")

if(MSVC)
    # ccache can't cache objects that write into a shared compiler PDB (/Zi): keep the debug info in each object (/Z7).
    # Needs policy CMP0141 (NEW from cmake_minimum_required 3.25 on); the linker still writes the DLL's PDB.
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
endif()

execute_process(COMMAND "${CCACHE_PROGRAM}" --version OUTPUT_VARIABLE _ccache_version ERROR_QUIET)
string(REGEX MATCH "[0-9]+\.[0-9]+(\.[0-9]+)?" _ccache_version "${_ccache_version}")
message(STATUS "ccache ${_ccache_version}: ${CCACHE_PROGRAM}")
