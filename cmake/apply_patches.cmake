# Applies cmake/patches/<prefix>*.patch to a fetched dependency the same way on every system:
#
#   cmake -DPATCH_DIR=<dir> -DPREFIX=<name prefix> -DWORK_DIR=<source dir> -DOUT_DIR=<scratch dir> -DGIT=<git>
#         -P apply_patches.cmake
#
# git apply wants the bytes to match, and line endings don't: Git on Windows (core.autocrlf) checks a dependency out
# with CRLF, on Linux with LF, and a patch file's own endings depend on how it was checked out. So the patches and the
# files they change are made LF first, then applied.

foreach(var PATCH_DIR PREFIX WORK_DIR OUT_DIR GIT)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "apply_patches.cmake: -D${var}=... is missing")
  endif()
endforeach()

file(GLOB patches "${PATCH_DIR}/${PREFIX}*.patch")
list(SORT patches)
file(MAKE_DIRECTORY "${OUT_DIR}")

foreach(patch IN LISTS patches)
  get_filename_component(name "${patch}" NAME)
  file(READ "${patch}" text)
  string(REPLACE "\r\n" "\n" text "${text}")
  set(lf "${OUT_DIR}/${name}")
  file(WRITE "${lf}" "${text}")

  # The files it changes: "+++ b/<path>" (a tab and a date may follow)
  string(REGEX MATCHALL "\n\\+\\+\\+ b/[^\t\n]+" targets "\n${text}")
  foreach(target IN LISTS targets)
    string(REGEX REPLACE "^\n\\+\\+\\+ b/" "" path "${target}")
    if(EXISTS "${WORK_DIR}/${path}")
      file(READ "${WORK_DIR}/${path}" content)
      string(REPLACE "\r\n" "\n" content "${content}")
      file(WRITE "${WORK_DIR}/${path}" "${content}")
    endif()
  endforeach()

  execute_process(COMMAND "${GIT}" apply --unsafe-paths "${lf}"
                  WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE rc ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${name} does not apply to ${WORK_DIR}:\n${err}")
  endif()
  message(STATUS "Applied ${name}")
endforeach()
