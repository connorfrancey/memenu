# --------------------------------------------------------------------
# Library versions + git commits
# --------------------------------------------------------------------

# yaml-cpp - https://github.com/jbeder/yaml-cpp
set(YAMLCPP_VERSION 0.9.0)
set(YAMLCPP_GIT_TAG 56e3bb550c91fd7005566f19c079cb7a503223cf)

# fmt - https://github.com/fmtlib/fmt
set(FMT_VERSION 12.2.0)   # UnrealLib's pin; spdlog uses this copy too (SPDLOG_FMT_EXTERNAL)

# spdlog - https://github.com/gabime/spdlog/
set(SPDLOG_VERSION v1.17.0)

# TOML11 - https://github.com/ToruNiina/toml11
set(TOML_VERSION v4.4.0)

# UnrealLib - https://github.com/connorfrancey/UnrealLib
set(UNREALLIB_GIT_TAG 91dcb10774b6cfe7a181478ebd97d439bf339e6f)   # main, M0-M9
set(LUA_VERSION v5.4.8)   # Mod scripting (src/runtime/lua_vm.cpp): built from source, as C++

# Tracy profiler client - https://github.com/wolfpld/tracy
# (MEMENU_TRACY). The Tracy profiler (the viewer) must be this same version.
set(TRACY_VERSION v0.14.1)   # Matches the installed Tracy profiler (0.14.1): the protocol versions must match

# Format.cmake - https://github.com/TheLartians/Format.cmake
set(FORMATDOTCMAKE_VERSION 1.8.2)

# CPM Licenses - https://github.com/cpm-cmake/CPMLicenses.cmake
set(CPM_LICENSES_VERSION 0.0.7)

# GoogleTest + GoogleBenchmark - https://github.com/google/googletest / https://github.com/google/benchmark
set(GOOGLETEST_VERSION v1.17.0)
set(GOOGLEBENCH_VERSION v1.10.0)

# mmultiplayer (old) - https://github.com/connorfrancey/mmultiplayer
set(MMULTIPLAYER_COMMIT 7b1eb25ddc0fc1ce498d024e7b2f9d1980b9eca3)

# ImGui-FileBrowser - https://github.com/AirGuanZ/imgui-filebrowser

# imjson - https://github.com/moleium/imjson
# TODO: Replace with custom imjson rewrite using toml11 (gh: connorfrancey/imtoml)
set(IMJSON_COMMIT 3130d41cadf8883e082f7cad14e52a9644146005)

# nlohmann/json - https://github.com/nlohmann/json
set(NLOHMANN_JSON_VERSION 3.12.0)

# zstd - https://github.com/meta/zstd
set(ZSTD_VERSION v1.5.7)

# ImGui - https://github.com/ocornut/imgui
# ImGui Addons:
#  - ImGuiFileBrowser: https://github.com/AirGuanZ/imgui-filebrowser
#  - ImGuiColorTextInput: https://github.com/BalazsJako/ImGuiColorTextEdit
#  - imgui_rich_md: https://github.com/pthom/imgui_rich_md
#  - ImGuiFileDialog: https://github.com/aiekick/ImGuiFileDialog
#  - imguizmo: https://github.com/CedricGuillemet/ImGuizmo
#  - implot:
#  - imcurve: https://github.com/jsoulier/imcurve
set(IMGUI_VERSION v1.92.9b)
set(IMGUIFILEBROWSER_GIT_COMMIT 47a1884524d6a3df5d99f70ecc82e1ed0ccd26cf)
set(IMGUICOLORTEXTINPUT_GIT_COMMIT ca2f9f1462e3b60e56351bc466acda448c5ea50d)
set(IMGUI_RICHMD_VERSION v1.0.0)
set(IMGUI_NODEEDITOR_VERSION v0.9.4)
set(IMPLOT_VERSION v1.1)
set(IMGUIZMO_VERSION 1.10)
set(IMGUIFILEDIALOG_VERSION v0.6.8)
set(IMCURVE_GIT_COMMIT 8cc8dc3a971593c6ce5aa8f418776545b2dbc3bb)     # https://github.com/jsoulier/imcurve/commit/8cc8dc3a971593c6ce5aa8f418776545b2dbc3bb

# MinHook - https://github.com/tsudakageyu/minhook
set(MINHOOK_VERSION v1.3.4)

# unordered_dense - https://github.com/martinus/unordered_dense
set(UNORDERED_DENSE_VERSION v5.3.3)

# magic_enum - https://github.com/Neargye/magic_enum
set(MAGICENUM_VERSION v0.9.8)

# nameof - https://github.com/Neargye/nameof
set(NAMEOF_VERSION v0.11.0)

# mimalloc - https://github.com/microsoft/mimalloc
set(MIMALLOC_VERSION v3.6.1)

set(ATOMICQUEUE_VERSION v1.9.4)

# stb (stb_image, stb_dxt) - https://github.com/nothings/stb (no tags: a commit)
set(STB_COMMIT 4e9b368bfc071e97206ea5c6adba68dd0cbf8897)
