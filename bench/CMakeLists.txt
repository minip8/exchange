# Google Benchmark microbenchmark suite (target: exchange_bench).
add_subdirectory(google)

# flash1-dev/matching-engine-benchmark adapter (skipped until the harness is
# fetched — see scripts/fetch_harness.sh).
add_subdirectory(flash1)

# Networking measured on the flash1 workload (target: net_workload_bench,
# Release only). Owns BenchClient.hpp, which loopback below also uses.
add_subdirectory(net)

# Networking latency micro-probe (target: net_loopback_bench, Release only).
add_subdirectory(loopback)
