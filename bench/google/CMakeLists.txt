include(FetchContent)

# Pin a specific tag for reproducible bench numbers across machines/CI runs.
FetchContent_Declare(
  googlebenchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG v1.9.5
)
# We don't need Google Benchmark's own test suite.
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googlebenchmark)

add_executable(exchange_bench
  bench_main.cpp
  bench_orderbook.cpp
  bench_matching_engine.cpp
  bench_ring.cpp
  bench_flash1_replay.cpp
  # The flash1 stream loader, shared with the network workload bench rather than
  # duplicated. It is compiled as a source of this target, NOT as a library:
  # a separate target that did not also link `debug_options` would hand a
  # std::vector across a _GLIBCXX_DEBUG boundary in Debug builds.
  ${CMAKE_CURRENT_SOURCE_DIR}/../net/Flash1Workload.cpp
)
target_include_directories(exchange_bench PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../net
)

# bench_ring.cpp needs the net headers, but nothing it touches has a .cpp —
# SpscRing, Command and Event are header-only, which is why net_protocol is
# an INTERFACE target and why this does not drag Boost in.
#
# benchmark_main is deliberately absent: bench_main.cpp supplies main() so the
# binary can accept --hist-json. See its header comment.
target_link_libraries(exchange_bench PRIVATE
  engine
  net_protocol
  benchmark::benchmark
)
find_package(Threads REQUIRED)
target_link_libraries(exchange_bench PRIVATE Threads::Threads)

target_compile_options(exchange_bench PRIVATE
  -O3 -march=native
)

# Regression gate example (wire into CI):
#   ./exchange_bench --benchmark_format=json > bench_results.json
#   compare against a checked-in baseline with google/benchmark's compare.py
