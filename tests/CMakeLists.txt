# Unit tests (GoogleTest) for the code that runs without the game: math, mesh building, TAS replays and macros, ...
# Run with `uv run memenu test` or `ctest --test-dir cmake-build-x86-debug --output-on-failure`.

add_executable(memenu_tests
        test_math.cpp
        test_mesh.cpp
        test_replay.cpp
        ${CMAKE_SOURCE_DIR}/src/render/mesh.cpp
        ${CMAKE_SOURCE_DIR}/src/tas/replay.cpp)
target_include_directories(memenu_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(memenu_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(memenu_tests PRIVATE GTest::gtest_main nlohmann_json::nlohmann_json)
if(MSVC)
  target_compile_options(memenu_tests PRIVATE /W4 /permissive- /utf-8)
endif()

# One CTest test per TEST(); listed when ctest runs (not at build time, so a build never runs the exe)
include(GoogleTest)
gtest_discover_tests(memenu_tests DISCOVERY_MODE PRE_TEST)
