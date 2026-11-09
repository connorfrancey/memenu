# Library versions are stored here
# --------------------------------------------------------------------

# yaml-cpp
set(YAMLCPP_VERSION 0.9.0)
set(YAMLCPP_GIT_TAG 56e3bb550c91fd7005566f19c079cb7a503223cf)

# MinHook
set(MINHOOK_VERSION v1.3.4)

# ImGui
set(IMGUI_VERSION v1.92.9b)

# fmt
set(FMT_VERSION 12.2.0)   # UnrealLib's pin; spdlog uses this copy too (SPDLOG_FMT_EXTERNAL)

# spdlog
set(SPDLOG_VERSION v1.17.0)

set(JSON_VERSION v3.11.3)

set(TOML_VERSION 4.4.0)

# UnrealLib (github.com/connorfrancey/UnrealLib)
set(UNREALLIB_GIT_TAG 91dcb10774b6cfe7a181478ebd97d439bf339e6f)   # main, M0-M9
set(LUA_VERSION v5.4.8)   # Mod scripting (src/runtime/lua_vm.cpp): built from source, as C++

# Tracy profiler client (MEMENU_TRACY). The Tracy profiler (the viewer) must be this same version.
set(TRACY_VERSION v0.14.1)   # Matches the installed Tracy profiler (0.14.1): the protocol versions must match
