# memenu loader: a dinput8.dll proxy that sits next to MirrorsEdge.exe and loads memenu\medge_menu.dll with the game.
# 32-bit like the game (the root CMakeLists enforces that). Installed by memenu_installer (installer/).

add_library(memenu_loader SHARED src/loader.cpp src/dinput8.def)
set_target_properties(memenu_loader PROPERTIES OUTPUT_NAME dinput8 PREFIX "")
target_compile_definitions(memenu_loader PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX MEMENU_VERSION="${MEMENU_VERSION}")

if(MSVC)
  target_compile_options(memenu_loader PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(memenu_loader PRIVATE -Wall -Wextra)
  target_link_options(memenu_loader PRIVATE -static -static-libgcc -static-libstdc++)
endif()
