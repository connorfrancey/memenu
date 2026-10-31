# medge launcher: starts (or waits for) Mirror's Edge and injects medge_menu.dll into that
# Must be 32-bit like the game and the DLL (the root CMakeLists already enforces that).

# Finding files (e.g. file(GLOB_RECUSE ...)) isn't necessary for this, so just add them manually
add_executable(medge_launcher src/main.cpp src/inject.cpp src/inject.h src/detect.cpp src/detect.h)

target_link_libraries(medge_launcher PRIVATE nlohmann_json::nlohmann_json shell32 advapi32)
target_compile_definitions(medge_launcher PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

if (MSVC)
    target_compile_options(medge_launcher PRIVATE /W4 /permissive- /utf-8)
    # Always run as administrator (UAC prompt), like the installer: injecting into the game needs the rights to open
    # its process, which a normal launcher does not have when the game itself runs elevated.
    target_link_options(medge_launcher PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
else()
    target_compile_options(medge_launcher PRIVATE -Wall -Wextra)
    target_link_options(medge_launcher PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()

# Build the DLL first and keep both side by side: the launcher looks for medge_menu.dll next to itself.
set_target_properties(medge_launcher PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_dependencies(medge_launcher medge_menu)
add_custom_command(TARGET medge_launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:medge_menu> $<TARGET_FILE_DIR:medge_launcher>
    COMMENT "Copying medge_menu.dll next to the launcher")