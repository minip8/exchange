# Google Benchmark microbenchmark suite (target: exchange_bench).
add_subdirectory(google)

# flash1-dev/matching-engine-benchmark adapter (skipped until the harness is
# fetched — see scripts/fetch_harness.sh).
add_subdirectory(flash1)

# Networking latency benchmark (target: net_loopback_bench, Release only).
add_subdirectory(loopback)
