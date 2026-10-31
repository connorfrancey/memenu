# memenu_installer: a single exe with the loader (dinput8.dll) and the menu (medge_menu.dll) embedded, which installs
# them into the game's Binaries folder. This is the file to hand to players.

# The payload: the built DLLs as RCDATA resources. The template is generated (so it can name the actual output files);
# the .rc that gets compiled is copied from it by a custom command that depends on both DLLs. That is what makes Ninja
# wait for them to be linked (and re-embed them whenever they change) before the resource compiler runs.
set(MEMENU_PAYLOAD_TEMPLATE ${CMAKE_CURRENT_BINARY_DIR}/payload_$<CONFIG>.rc.in)
set(MEMENU_PAYLOAD_RC ${CMAKE_CURRENT_BINARY_DIR}/payload_$<CONFIG>.rc)
file(GENERATE OUTPUT ${MEMENU_PAYLOAD_TEMPLATE} CONTENT
"#include \"${CMAKE_CURRENT_SOURCE_DIR}/src/resource.h\"
IDR_LOADER_DLL RCDATA \"$<TARGET_FILE:memenu_loader>\"
IDR_MENU_DLL   RCDATA \"$<TARGET_FILE:medge_menu>\"
")
add_custom_command(
        OUTPUT ${MEMENU_PAYLOAD_RC}
        COMMAND ${CMAKE_COMMAND} -E copy ${MEMENU_PAYLOAD_TEMPLATE} ${MEMENU_PAYLOAD_RC}
        DEPENDS ${MEMENU_PAYLOAD_TEMPLATE} memenu_loader medge_menu
        COMMENT "Embedding dinput8.dll and medge_menu.dll into the installer"
        VERBATIM)

add_executable(memenu_installer WIN32
        src/main.cpp src/install.cpp src/install.h src/resource.h
        ${CMAKE_SOURCE_DIR}/launcher/src/detect.cpp ${CMAKE_SOURCE_DIR}/launcher/src/detect.h
        ${MEMENU_PAYLOAD_RC})
target_include_directories(memenu_installer PRIVATE ${CMAKE_SOURCE_DIR}/launcher/src)
target_compile_definitions(memenu_installer PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
        MEMENU_VERSION="${MEMENU_VERSION}")
target_link_libraries(memenu_installer PRIVATE comctl32 comdlg32 shell32 advapi32)

add_dependencies(memenu_installer memenu_loader medge_menu)

set_target_properties(memenu_installer PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

if(MSVC)
  target_compile_options(memenu_installer PRIVATE /W4 /permissive- /utf-8)
  # The game lives in Program Files: writing there needs admin rights, so ask for them up front.
  target_link_options(memenu_installer PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
else()
  target_compile_options(memenu_installer PRIVATE -Wall -Wextra)
  target_link_options(memenu_installer PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()
