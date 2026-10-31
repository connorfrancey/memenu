# Unit tests (Catch2) for the code that runs without the game: math, mesh building, ...
# Run with `uv run memenu test` or `ctest --test-dir cmake-build-x86-debug --output-on-failure`.

add_executable(memenu_tests
        test_math.cpp
        test_mesh.cpp
        ${CMAKE_SOURCE_DIR}/src/render/mesh.cpp)
target_include_directories(memenu_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(memenu_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(memenu_tests PRIVATE Catch2::Catch2WithMain)
if(MSVC)
  target_compile_options(memenu_tests PRIVATE /W4 /permissive- /utf-8)
endif()

include(${catch2_SOURCE_DIR}/extras/Catch.cmake)
catch_discover_tests(memenu_tests)
