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
  bench_orderbook.cpp
  bench_matching_engine.cpp
)

target_link_libraries(exchange_bench PRIVATE
  engine
  benchmark::benchmark
  benchmark::benchmark_main
)

target_compile_options(exchange_bench PRIVATE
  -O3 -march=native
)

# Regression gate example (wire into CI):
#   ./exchange_bench --benchmark_format=json > bench_results.json
#   compare against a checked-in baseline with google/benchmark's compare.py

# flash1-dev/matching-engine-benchmark adapter (skipped until the harness is
# fetched — see scripts/fetch_harness.sh).
add_subdirectory(flash1)