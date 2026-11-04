# Unit tests (GoogleTest) for the code that runs without the game: math, mesh building, TAS replays and macros, ...
# Run with `uv run memenu test` or `ctest --test-dir cmake-build-x86-debug --output-on-failure`.

add_executable(memenu_tests
        test_math.cpp
        test_mesh.cpp
        test_replay.cpp
        test_timeline.cpp
        test_speedrun.cpp
        test_geometry.cpp
        test_history.cpp
        test_input_layout.cpp
        test_settings.cpp
        ${CMAKE_SOURCE_DIR}/src/render/mesh.cpp
        ${CMAKE_SOURCE_DIR}/src/tas/replay.cpp
        ${CMAKE_SOURCE_DIR}/src/tas/timeline.cpp
        ${CMAKE_SOURCE_DIR}/src/speedrun/timer.cpp
        ${CMAKE_SOURCE_DIR}/src/tas/history.cpp
        ${CMAKE_SOURCE_DIR}/src/features/visual/input_layout.cpp
        # The settings framework and what it needs (test_settings.cpp): no game, no window
        ${CMAKE_SOURCE_DIR}/src/core/setting.cpp
        ${CMAKE_SOURCE_DIR}/src/core/module.cpp
        ${CMAKE_SOURCE_DIR}/src/core/notify.cpp
        ${CMAKE_SOURCE_DIR}/src/core/keybinds.cpp
        ${CMAKE_SOURCE_DIR}/src/core/keys.cpp
        ${CMAKE_SOURCE_DIR}/src/core/scheduler.cpp
        ${CMAKE_SOURCE_DIR}/src/core/clock.cpp
        ${CMAKE_SOURCE_DIR}/src/core/cache.cpp)
target_include_directories(memenu_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(memenu_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
        MEMENU_EXAMPLES_DIR="${CMAKE_SOURCE_DIR}/examples")
target_link_libraries(memenu_tests PRIVATE GTest::gtest_main nlohmann_json::nlohmann_json imgui spdlog::spdlog)
if(MSVC)
  target_compile_options(memenu_tests PRIVATE /W4 /permissive- /utf-8)
endif()

# One CTest test per TEST(); listed when ctest runs (not at build time, so a build never runs the exe)
include(GoogleTest)
gtest_discover_tests(memenu_tests DISCOVERY_MODE PRE_TEST)

# The installer's work on a game folder, on a fake one (tests/installer/test_install.cpp): stand-in DLLs embedded the
# way the real installer embeds the built ones, as RCDATA resources (written with absolute paths for the resource
# compiler).
set(MEMENU_TEST_PAYLOAD_RC ${CMAKE_CURRENT_BINARY_DIR}/installer_payload.rc)
file(WRITE ${MEMENU_TEST_PAYLOAD_RC}
"#include \"${CMAKE_SOURCE_DIR}/installer/src/resource.h\"
IDR_LOADER_DLL RCDATA \"${CMAKE_CURRENT_SOURCE_DIR}/installer/fake_loader.bin\"
IDR_MENU_DLL   RCDATA \"${CMAKE_CURRENT_SOURCE_DIR}/installer/fake_menu.bin\"
")
add_executable(installer_tests
        installer/test_install.cpp
        ${CMAKE_SOURCE_DIR}/installer/src/install.cpp
        ${CMAKE_SOURCE_DIR}/launcher/src/detect.cpp
        ${MEMENU_TEST_PAYLOAD_RC})
target_include_directories(installer_tests PRIVATE ${CMAKE_SOURCE_DIR}/installer/src ${CMAKE_SOURCE_DIR}/launcher/src)
target_compile_definitions(installer_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE MEMENU_VERSION="test"
        MEMENU_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/installer")
target_link_libraries(installer_tests PRIVATE GTest::gtest_main shell32 advapi32)
if(MSVC)
  target_compile_options(installer_tests PRIVATE /W4 /permissive- /utf-8)
endif()
gtest_discover_tests(installer_tests DISCOVERY_MODE PRE_TEST)
