#pragma once

/*
The seam between `bench_main.cpp` (which owns argv) and
`bench_flash1_replay.cpp` (which owns the measurement). Nothing else in the
suite needs either.

Options are filled by main() *before* `RunSpecifiedBenchmarks()` and read lazily
by the benchmark bodies, so registration stays at file scope next to the
benchmarks — the same shape as the rest of the suite — and does not have to wait
for the command line to be parsed.
*/

#include <cstdint>
#include <string>

namespace Exchange::Bench {

struct Flash1Options {
  // Empty means "resolve it": $EXCHANGE_HARNESS_DIR, then a walk up from
  // /proc/self/exe, then a relative path. bench_pipeline.sh invokes the binary
  // by absolute path with no cd, so a relative default cannot be trusted.
  std::string harness_dir{};
  // Empty means "do not write a sidecar".
  std::string hist_json{};
  uint64_t count{0};  // 0 -> kCanonicalCount
  uint32_t seed{0};   // 0 -> kCanonicalSeed
};

Flash1Options& flash1Options();

// Writes the per-operation latency distributions collected by whichever flash1
// benchmarks actually ran. Always produces a valid document, including when
// nothing ran ("runs": []) — the pipeline hands this path in unconditionally.
bool writeHistJson(const std::string& path);

}  // namespace Exchange::Bench
