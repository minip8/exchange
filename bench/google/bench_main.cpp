/*
`main()` for exchange_bench.

This replaces `benchmark::benchmark_main` for exactly one reason: the flash1
replay benchmark writes a per-operation latency distribution, which does not fit
in Google Benchmark's output (v1.9.5 has no percentile statistics, and a
user-defined statistic sees one value per *repetition*, not per operation). So
it gets a sidecar file, and a sidecar file needs a flag, and BENCHMARK_MAIN's
`ReportUnrecognizedArguments` rejects any flag it does not own.

Argument order is load-bearing in both directions. `benchmark::Initialize` must
run first, or GB's own flags reach our parser. Our parser must then *compact
argv in place*, or the flags it consumed are still there when
`ReportUnrecognizedArguments` looks and the process exits 1 having run nothing.
*/
#include <benchmark/benchmark.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "Flash1Bench.hpp"

namespace {

constexpr const char* kUsage =
    "exchange_bench — engine microbenchmarks (Google Benchmark).\n"
    "\n"
    "In addition to every --benchmark_* flag, this binary accepts:\n"
    "  --hist-json PATH      write the flash1 per-op latency distribution "
    "here\n"
    "  --harness-dir PATH    flash1 harness checkout (default: found relative\n"
    "                        to the executable, or $EXCHANGE_HARNESS_DIR)\n"
    "  --hist-count N        generator order count   (default 1000000)\n"
    "  --hist-seed N         generator seed          (default 23)\n"
    "\n"
    "The flash1 benchmarks are opt-in and are not part of the default suite:\n"
    "  exchange_bench --benchmark_filter='^BM_Flash1_' --hist-json "
    "/tmp/h.json\n";

bool wantsValue(const char* flag, std::string& out, int& index, int argc,
                char** argv) {
  if (index + 1 >= argc) {
    std::fprintf(stderr, "error: %s needs a value\n", flag);
    return false;
  }
  out = argv[++index];
  return true;
}

/*
Consumes our flags and leaves argv holding only what we did not recognise, so
Google Benchmark's own unrecognised-argument check still does its job on the
remainder.
*/
bool parseFlash1Args(int& argc, char** argv) {
  using Exchange::Bench::flash1Options;
  int out{1};
  std::string value{};
  for (int i{1}; i < argc; ++i) {
    const char* arg{argv[i]};
    if (std::strcmp(arg, "--hist-json") == 0) {
      if (!wantsValue(arg, value, i, argc, argv)) return false;
      flash1Options().hist_json = value;
    } else if (std::strcmp(arg, "--harness-dir") == 0) {
      if (!wantsValue(arg, value, i, argc, argv)) return false;
      flash1Options().harness_dir = value;
    } else if (std::strcmp(arg, "--hist-count") == 0) {
      if (!wantsValue(arg, value, i, argc, argv)) return false;
      flash1Options().count = std::strtoull(value.c_str(), nullptr, 10);
    } else if (std::strcmp(arg, "--hist-seed") == 0) {
      if (!wantsValue(arg, value, i, argc, argv)) return false;
      flash1Options().seed =
          static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    } else {
      argv[out++] = argv[i];
    }
  }
  argc = out;
  argv[argc] = nullptr;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  // benchmark::Initialize prints its own usage and exits(0) on --help, so ours
  // has to be out the door before it is called, not after.
  for (int i{1}; i < argc; ++i) {
    if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
      std::fputs(kUsage, stdout);
      break;
    }
  }
  benchmark::Initialize(&argc, argv);
  if (!parseFlash1Args(argc, argv)) return 2;
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;

  const std::size_t ran{benchmark::RunSpecifiedBenchmarks()};
  benchmark::Shutdown();

  const std::string& hist_json{Exchange::Bench::flash1Options().hist_json};
  if (!hist_json.empty()) {
    // Written unconditionally, including when no flash1 benchmark ran: the
    // pipeline always passes the path and always reads the file back.
    if (!Exchange::Bench::writeHistJson(hist_json)) {
      std::fprintf(stderr, "error: could not write %s\n", hist_json.c_str());
      return 1;
    }
    std::printf("hist json: %s\n", hist_json.c_str());
  }
  return ran == 0 ? 1 : 0;
}
