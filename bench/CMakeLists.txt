# Benchmarks (Google Benchmark) for the code that runs without the game: replay recording, hashing, saving and loading,
# macros, math and mesh building. Run with `uv run memenu bench` (a release build: debug timings mean little).

add_executable(memenu_bench
        bench_main.cpp
        bench_replay.cpp
        bench_render.cpp
        ${CMAKE_SOURCE_DIR}/src/render/mesh.cpp
        ${CMAKE_SOURCE_DIR}/src/tas/replay.cpp)
target_include_directories(memenu_bench PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(memenu_bench PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
        MEMENU_EXAMPLES_DIR="${CMAKE_SOURCE_DIR}/examples")
target_link_libraries(memenu_bench PRIVATE benchmark::benchmark nlohmann_json::nlohmann_json magic_enum::magic_enum unordered_dense::unordered_dense)
set_target_properties(memenu_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
if(MSVC)
  target_compile_options(memenu_bench PRIVATE /W4 /permissive- /utf-8)
endif()
