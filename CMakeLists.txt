cmake_minimum_required(VERSION 3.20)
project(exchange CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Compiler flags ────────────────────────────────────────────────────────────
add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)

# Generator expressions with multiple flags must be split — CMake passes each
# list element as a separate argument, so combining flags in one string breaks.
add_compile_options(
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Release>:-march=native>
    $<$<CONFIG:Release>:-DNDEBUG>
    $<$<CONFIG:Debug>:-g>
    $<$<CONFIG:Debug>:-O0>
    $<$<CONFIG:Debug>:-fsanitize=address>
    $<$<CONFIG:Debug>:-fsanitize=undefined>
)
add_link_options(
    $<$<CONFIG:Debug>:-fsanitize=address>
    $<$<CONFIG:Debug>:-fsanitize=undefined>
)

# ── Interface library (headers only) ─────────────────────────────────────────
add_library(exchange_lib INTERFACE)
target_include_directories(exchange_lib INTERFACE ${CMAKE_SOURCE_DIR}/include)

# ── Main CLI binary ───────────────────────────────────────────────────────────
add_executable(engine src/main.cpp)
target_link_libraries(engine PRIVATE exchange_lib)

# ── Tests ─────────────────────────────────────────────────────────────────────
enable_testing()

find_package(GTest REQUIRED)

add_executable(tests
    test/test_matching.cpp
    test/test_arena.cpp
    test/test_multi_market.cpp
)
target_link_libraries(tests PRIVATE exchange_lib GTest::GTest GTest::Main)
# Tests always use BackoffPolicy (no CONSUMER_POLICY_SPIN) to be CI-friendly.

add_test(NAME all_tests COMMAND tests)

# ── Benchmark ─────────────────────────────────────────────────────────────────
# Build Google Benchmark from source so it compiles with the same Release flags
# as the engine. The system libbenchmark-dev package ships as Debug, which skews
# timing measurements. FetchContent gives us a proper -O3 build.
include(FetchContent)
FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.8.3
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googlebenchmark)

add_executable(bench bench/bench_throughput.cpp)
target_link_libraries(bench PRIVATE exchange_lib benchmark::benchmark)
# Run in Release mode only — Debug numbers are meaningless.
set_target_properties(bench PROPERTIES EXCLUDE_FROM_ALL $<CONFIG:Debug>)
