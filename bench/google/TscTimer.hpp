#pragma once

/*
Per-operation timing for the flash1 replay benchmark.

`BenchSupport.hpp` rule 2 forbids clock reads inside a timed region because a
`now()` costs ~25 ns against operations that take 40-300 ns. That rule stands
everywhere except here, where the per-operation time *is* the measurement — see
the header comment of bench_flash1_replay.cpp. What this header does is make the
tax as small as it can be and, more importantly, *measurable*: `rdtsc` instead
of a vDSO call, and `calibrate().overhead` holding the distribution of an empty
start/stop pair taken with these exact functions.

Ticks are converted to nanoseconds once, at report time. If the TSC cannot be
trusted to tick at a constant rate, `ns_per_tick` is left at 0 and the caller
must publish ticks — a frequency-scaled TSC yields a histogram whose shape is
perfectly plausible and whose scale is a lie, and nothing downstream can detect
that.
*/

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#include <x86intrin.h>
#define EXCHANGE_BENCH_HAVE_RDTSC 1
#else
#include <ctime>
#define EXCHANGE_BENCH_HAVE_RDTSC 0
#endif

#include "LatencyHistogram.hpp"

namespace Exchange::Bench {

#if EXCHANGE_BENCH_HAVE_RDTSC

/*
`lfence` before and after the read keeps the surrounding work from drifting
across it: without the leading fence the operation under test can be hoisted
above the timestamp, and without the trailing one the next operation's work can
sink into the measured window. `rdtscp` on the stop side additionally waits for
everything before it to retire. GCC's `_mm_lfence` carries a memory clobber, so
no separate compiler barrier is needed.
*/
inline uint64_t tscStart() noexcept {
  _mm_lfence();
  const uint64_t t{__rdtsc()};
  _mm_lfence();
  return t;
}

inline uint64_t tscStop() noexcept {
  unsigned aux{};
  const uint64_t t{__rdtscp(&aux)};
  _mm_lfence();
  return t;
}

// CPUID leaf 0x80000007, EDX bit 8: the TSC ticks at a constant rate regardless
// of P-state and does not stop in C-states.
inline bool hasCpuidInvariantTsc() noexcept {
  if (__get_cpuid_max(0x80000000u, nullptr) < 0x80000007u) return false;
  unsigned eax{}, ebx{}, ecx{}, edx{};
  if (__get_cpuid(0x80000007u, &eax, &ebx, &ecx, &edx) == 0) return false;
  return (edx & (1u << 8)) != 0;
}

#else

inline uint64_t tscStart() noexcept {
  timespec ts{};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull +
         static_cast<uint64_t>(ts.tv_nsec);
}
inline uint64_t tscStop() noexcept { return tscStart(); }
inline bool hasCpuidInvariantTsc() noexcept { return false; }

#endif

struct TimerCalibration {
  // "rdtsc" | "rdtsc_unverified" | "clock_gettime"
  std::string source{};
  bool invariant_tsc{false};
  // Which evidence established that — see invariantTscEvidence().
  std::string invariant_evidence{};
  std::string clocksource{};
  // 0 means "do not convert" — the caller must report ticks.
  double ns_per_tick{0.0};
  double calibration_spread_ppm{0.0};
  int calibration_rounds{0};
  LatencyHistogram overhead{};
  /*
  The instrument's real granularity, which is NOT one tick. The TSC is commonly
  derived from a slower reference and scaled, so it advances in steps — this
  machine steps ~39 counts (~10 ns) at a nominal 0.26 ns/tick. Measured as the
  GCD of the empty-pair deltas, and reported because a histogram bucket finer
  than this cannot be occupied: without it the low end of the distribution looks
  like noise rather than like quantisation.
  */
  uint64_t resolution_ticks{1};

  double toNs(uint64_t ticks) const noexcept {
    return ns_per_tick == 0.0 ? static_cast<double>(ticks)
                              : static_cast<double>(ticks) * ns_per_tick;
  }
  bool inNanoseconds() const noexcept { return ns_per_tick != 0.0; }
};

namespace Detail {

inline bool cpuFlag(std::string_view flag) {
  std::ifstream in{"/proc/cpuinfo"};
  std::string line{};
  while (std::getline(in, line)) {
    if (line.rfind("flags", 0) != 0) continue;
    const std::string needle{std::string{" "} + std::string{flag} + " "};
    return (line + " ").find(needle) != std::string::npos;
  }
  return false;
}

/*
Whether ticks can be treated as time, and on what grounds.

The CPUID bit is the direct answer, but a hypervisor may mask leaf 0x80000007
while still presenting a perfectly stable TSC — WSL2/Hyper-V, this project's
stated platform, does exactly that: the leaf reads back zero, yet the kernel
reports `tsc_known_freq` and drives a TSC-page clocksource. Refusing to convert
there would leave the benchmark reporting ticks on the one machine it actually
runs on, so kernel corroboration counts as evidence too.

What is *not* accepted is silence. With no evidence at all the histogram stays
in ticks and says so, because a frequency-scaled TSC produces a histogram whose
shape is entirely plausible and whose scale is a lie.
*/
inline std::pair<bool, std::string> invariantTscEvidence() {
  if (hasCpuidInvariantTsc()) return {true, "cpuid.0x80000007.edx[8]"};
  if (cpuFlag("constant_tsc") && cpuFlag("nonstop_tsc"))
    return {true, "cpuinfo.constant_tsc+nonstop_tsc"};
  if (cpuFlag("tsc_known_freq")) return {true, "cpuinfo.tsc_known_freq"};
  return {false, "none"};
}

inline std::string readClocksource() {
  std::ifstream in{
      "/sys/devices/system/clocksource/clocksource0/current_clocksource"};
  std::string value{};
  if (in) std::getline(in, value);
  return value.empty() ? std::string{"unknown"} : value;
}

// One round: busy-wait a fixed wall interval and see how many ticks elapsed.
// Busy-wait rather than sleep, so the measurement never spans a C-state a
// non-invariant TSC would have stopped through.
inline double calibrationRound(std::chrono::nanoseconds window) {
  using Clock = std::chrono::steady_clock;
  const auto wall_begin{Clock::now()};
  const uint64_t tick_begin{tscStart()};
  while (Clock::now() - wall_begin < window) {
  }
  const uint64_t tick_end{tscStop()};
  const auto wall_end{Clock::now()};

  const auto elapsed_ns{std::chrono::duration_cast<std::chrono::nanoseconds>(
                            wall_end - wall_begin)
                            .count()};
  const uint64_t elapsed_ticks{tick_end - tick_begin};
  if (elapsed_ticks == 0) return 0.0;
  return static_cast<double>(elapsed_ns) / static_cast<double>(elapsed_ticks);
}

inline LatencyHistogram measureTimerOverhead(std::size_t samples,
                                             uint64_t& resolution_ticks) {
  LatencyHistogram histogram{};
  uint64_t step{0};
  for (std::size_t i{0}; i < samples; ++i) {
    const uint64_t begin{tscStart()};
    // Nothing between the two reads: what is left is the instrument itself.
    const uint64_t end{tscStop()};
    if (end < begin) continue;
    const uint64_t delta{end - begin};
    histogram.record(delta);
    if (delta != 0) step = std::gcd(step, delta);
  }
  resolution_ticks = step == 0 ? 1 : step;
  return histogram;
}

inline TimerCalibration runCalibration() {
  TimerCalibration calibration{};
  calibration.clocksource = readClocksource();

#if EXCHANGE_BENCH_HAVE_RDTSC
  const auto [invariant, evidence]{invariantTscEvidence()};
  calibration.invariant_tsc = invariant;
  calibration.invariant_evidence = evidence;
  calibration.source = invariant ? "rdtsc" : "rdtsc_unverified";
#else
  calibration.source = "clock_gettime";
  calibration.invariant_evidence = "n/a";
#endif

  // Three 50 ms rounds: enough that a ~20 ns clock read is a 4e-7 error, short
  // enough that the whole thing is invisible against a 2M-message replay.
  constexpr int kRounds{3};
  constexpr auto kWindow{std::chrono::milliseconds{50}};
  double rounds[kRounds]{};
  for (int i{0}; i < kRounds; ++i) rounds[i] = calibrationRound(kWindow);
  calibration.calibration_rounds = kRounds;

  double lo{rounds[0]}, hi{rounds[0]}, mid{rounds[0]};
  for (int i{1}; i < kRounds; ++i) {
    lo = rounds[i] < lo ? rounds[i] : lo;
    hi = rounds[i] > hi ? rounds[i] : hi;
  }
  // Median of three without a sort.
  mid = rounds[0] + rounds[1] + rounds[2] - lo - hi;
  if (mid > 0.0) {
    calibration.calibration_spread_ppm = (hi - lo) / mid * 1e6;
  }

#if EXCHANGE_BENCH_HAVE_RDTSC
  // Only publish a conversion factor when the ticks are known to be uniform.
  // Otherwise the histogram stays in ticks and says so.
  if (calibration.invariant_tsc) calibration.ns_per_tick = mid;
#else
  // The fallback path already returns nanoseconds.
  calibration.ns_per_tick = 1.0;
#endif

  calibration.overhead =
      measureTimerOverhead(100'000, calibration.resolution_ticks);
  return calibration;
}

}  // namespace Detail

/*
Calibrated once, lazily, on first use — so a run filtered to benchmarks that do
not time anything never pays the ~150 ms.
*/
inline const TimerCalibration& calibratedTimer() {
  static const TimerCalibration calibration{Detail::runCalibration()};
  return calibration;
}

}  // namespace Exchange::Bench
