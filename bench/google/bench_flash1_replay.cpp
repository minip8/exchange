/*
The flash1 order stream, replayed through OrderBook, timed per message.

Everything else in this suite measures a synthetic fixture — a book built to a
chosen depth, then one operation repeated — and reports a mean. This one replays
the *actual* stream the flash1 conformance harness runs (five volatility
regimes, real order lifecycles with cancels and modifies) and reports the
per-operation latency *distribution*. That is the view that separates "the
median add is 60 ns" from "one add in a thousand sweeps 40 levels and costs
9 us", which no aggregate in either suite can show.

RULE 2 OF BenchSupport.hpp IS DELIBERATELY BROKEN HERE. That rule — no clock
reads inside a timed region — exists because a `now()` costs ~25 ns against
operations taking 40-300 ns. Here the per-operation time *is* the measurement,
so the read cannot be hoisted out; what can be done is to make it as cheap as
possible and, crucially, to measure it:

  * `TscTimer.hpp` uses fenced `rdtsc`/`rdtscp`, not a vDSO call, and publishes
    the distribution of an empty start/stop pair taken with the same functions.
    The plot draws that as a "timer floor" line: everything left of it is
    instrument, not engine.
  * Every benchmark here has a `_NoTiming` twin running the identical replay
    with `if constexpr (kInstrument) == false`. The difference between the two
    is the instrumentation tax, measured rather than assumed. Note the twin is a
    *lower* bound: removing the fences also lets the compiler reorder across
    operation boundaries, which the instrumented version forbids.

So: THE AGGREGATE ns/op THIS BENCHMARK REPORTS IS NOT THE UNINSTRUMENTED
PER-OPERATION COST. The `_NoTiming` row beside it is.

The replay semantics are byte-identical to `bench/flash1/adapter.cpp:131-179`,
which is the harness contract and the reference — including the explicit-id
`Order` constructor (the one-minter rule), the IOC residual pull, and modify as
remove + re-add rather than `OrderBook::modifyOrder`. If the two ever diverge,
these numbers stop describing the thing the conformance harness measures. Do
not "improve" one without the other.

Opt-in, because a 2M-message replay does not belong in the default suite:

    cmake --build --preset bench
    ./build/release/bench/google/exchange_bench \
      --benchmark_filter='^BM_Flash1_' \
      --harness-dir "$PWD/external/matching-engine-benchmark" \
      --hist-json /tmp/hist.json
*/
#include <benchmark/benchmark.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Flash1Bench.hpp"
#include "Flash1Workload.hpp"
#include "LatencyHistogram.hpp"
#include "TscTimer.hpp"
#include "engine/Fill.hpp"
#include "engine/Order.hpp"
#include "engine/OrderBook.hpp"
#include "types/OrderId.hpp"
#include "types/OrderPrice.hpp"
#include "types/OrderQuantity.hpp"
#include "types/OrderSide.hpp"
#include "types/OrderTime.hpp"
#include "types/Symbol.hpp"

using namespace Exchange::Bench;
using namespace Exchange::Engine;
using namespace Exchange::Types;

namespace {

// The harness's five volatility regimes, in the order run_flash1.sh uses.
constexpr std::array<std::string_view, 5> kScenarios{
    "static", "normal", "swing-25", "swing-40", "flash-crash"};

/*
Kinds are split because pooling them produces a multimodal blob in which a
cancel's hash lookup and a 40-level sweep sit in the same curve, and the whole
point of a distribution is to see which operation owns the tail.
*/
enum class OpKind : std::size_t { New = 0, NewIoc = 1, Cancel = 2, Modify = 3 };
constexpr std::size_t kOpKindCount{4};
constexpr std::array<std::string_view, kOpKindCount> kOpKindNames{
    "new", "new_ioc", "cancel", "modify"};

using HistSet = std::array<LatencyHistogram, kOpKindCount>;

struct Counters {
  uint64_t fills{0};
  // Failures are expected, not errors: the generator plants ~2% duplicate
  // cancels and stale modifies. Zero of them means the stream did not replay.
  uint64_t cancel_rejects{0};
  uint64_t modify_rejects{0};
  // A backwards TSC delta means the thread migrated to a core whose TSC is not
  // synchronised with the first one. Counted, never recorded.
  uint64_t timer_anomalies{0};
};

// --- the adapter's mapping, reproduced exactly ------------------------------

OrderTime timeFromSeq(uint64_t seq) noexcept {
  using TimePoint = OrderTime::T;
  return OrderTime{TimePoint{TimePoint::duration{static_cast<int64_t>(seq)}}};
}
OrderSide sideFrom(uint8_t side) noexcept {
  return side == 0 ? OrderSide::Buy : OrderSide::Sell;
}
OpKind kindOf(const WorkloadRecord& record) noexcept {
  switch (record.type) {
    case MsgKind::kCancel:
      return OpKind::Cancel;
    case MsgKind::kModify:
      return OpKind::Modify;
    default:
      return record.ioc != 0 ? OpKind::NewIoc : OpKind::New;
  }
}

/*
One pass over the stream. `kInstrument` compiles the timing away entirely for
the twin, so the two differ by exactly the instrument and nothing else.

Note what is and is not inside a timed span. The `std::vector<Fill>` that
`addOrder` returns is constructed *and destroyed* inside it, because the adapter
pays that allocation too and hoisting it would make the two paths incomparable —
see the note on bimodality in the docs. `LatencyHistogram::record` runs between
one operation's stop and the next one's start, so it is in no sample; it is in
the wall-clock aggregate, which is what the twin exists to expose.
*/
template <bool kInstrument>
Counters replayStream(OrderBook& book,
                      const std::vector<WorkloadRecord>& records,
                      HistSet& histograms) {
  Counters counters{};
  for (const WorkloadRecord& record : records) {
    const OpKind kind{kindOf(record)};

    uint64_t begin{0};
    if constexpr (kInstrument) begin = tscStart();

    switch (record.type) {
      case MsgKind::kCancel: {
        if (!book.removeOrder(OrderId{record.order_id}).has_value())
          ++counters.cancel_rejects;
      } break;

      case MsgKind::kModify: {
        // Cancel + reinsert at the new price/quantity, losing queue priority —
        // the harness contract. NOT OrderBook::modifyOrder, which takes neither
        // a new price nor a new quantity.
        if (!book.removeOrder(OrderId{record.order_id}).has_value()) {
          ++counters.modify_rejects;
          break;
        }
        const std::vector<Fill> fills{book.addOrder(
            Order{OrderId{record.order_id},
                  OrderPrice{encodePrice(record.price_ticks)},
                  timeFromSeq(record.seq), OrderQuantity{record.quantity},
                  sideFrom(record.side)})};
        counters.fills += fills.size();
      } break;

      default: {
        const std::vector<Fill> fills{book.addOrder(
            Order{OrderId{record.order_id},
                  OrderPrice{encodePrice(record.price_ticks)},
                  timeFromSeq(record.seq), OrderQuantity{record.quantity},
                  sideFrom(record.side)})};
        counters.fills += fills.size();
        if (record.ioc != 0) {
          uint64_t filled{0};
          for (const Fill& fill : fills) filled += fill.quantity.value;
          // addOrder rested the remainder; an IOC takes it straight back out,
          // and that removal is part of handling this one message.
          if (filled < record.quantity)
            (void)book.removeOrder(OrderId{record.order_id});
        }
      } break;
    }

    if constexpr (kInstrument) {
      const uint64_t end{tscStop()};
      if (end >= begin) [[likely]] {
        histograms[static_cast<std::size_t>(kind)].record(end - begin);
      } else {
        ++counters.timer_anomalies;
      }
    }
  }
  return counters;
}

// --- what the sidecar is assembled from -------------------------------------

struct HistRun {
  std::string benchmark{};
  std::string scenario{};
  bool instrumented{false};
  uint64_t repetitions{0};
  uint64_t messages{0};         // per repetition
  double total_replay_ns{0.0};  // summed over repetitions
  uint64_t total_messages{0};   // summed over repetitions
  Counters counters{};
  HistSet by_kind{};
};

std::vector<HistRun>& histRegistry() {
  static std::vector<HistRun> registry{};
  return registry;
}

// Repeated invocations of one benchmark (--benchmark_repetitions=N) merge into
// a single entry: more samples make a better tail, and the repetition count is
// recorded so a reader knows what a bucket count covers.
HistRun& registryEntry(const std::string& name) {
  for (HistRun& run : histRegistry())
    if (run.benchmark == name) return run;
  histRegistry().push_back(HistRun{.benchmark = name});
  return histRegistry().back();
}

// Set the first time a benchmark actually calibrates, so a run in which nothing
// timed anything does not pay ~150 ms just to write "timer": {...}.
bool g_timer_used{false};

// --- workload loading -------------------------------------------------------

std::filesystem::path resolveHarnessDir() {
  const Flash1Options& options{flash1Options()};
  if (!options.harness_dir.empty()) return options.harness_dir;
  if (const char* from_env{std::getenv("EXCHANGE_HARNESS_DIR")};
      from_env != nullptr && *from_env != '\0')
    return from_env;

  // bench_pipeline.sh invokes this binary by absolute path with no cd, so CWD
  // proves nothing. The build tree does: walk up from the executable looking
  // for the checkout.
  std::error_code error{};
  const std::filesystem::path self{
      std::filesystem::read_symlink("/proc/self/exe", error)};
  if (!error) {
    for (std::filesystem::path dir{self.parent_path()}; !dir.empty();
         dir = dir.parent_path()) {
      const std::filesystem::path candidate{
          dir / "external/matching-engine-benchmark"};
      if (std::filesystem::exists(candidate, error)) return candidate;
      if (!dir.has_relative_path()) break;
    }
  }
  return std::filesystem::path{"external/matching-engine-benchmark"};
}

struct LoadedWorkload {
  std::vector<WorkloadRecord> records{};
  std::string error{};
};

// Both twins of a scenario share one load, and a filtered single-scenario run
// reads exactly one file.
const LoadedWorkload& cachedWorkload(std::string_view scenario) {
  static std::map<std::string, LoadedWorkload, std::less<>> cache{};
  if (const auto it{cache.find(scenario)}; it != cache.end()) return it->second;

  const Flash1Options& options{flash1Options()};
  auto loaded{
      ensureWorkload(resolveHarnessDir(), scenario,
                     options.seed == 0 ? kCanonicalSeed : options.seed,
                     options.count == 0 ? kCanonicalCount : options.count)};

  LoadedWorkload entry{};
  if (loaded.has_value())
    entry.records = std::move(loaded).value();
  else
    entry.error = std::move(loaded).error();
  return cache.emplace(std::string{scenario}, std::move(entry)).first->second;
}

// --- the benchmark ----------------------------------------------------------

/*
A plain `if` on a compile-time constant, not an `#if` around the body: the
preprocessor version leaves everything below it unreferenced in a Debug build,
which is both a pile of -Wunused-function warnings and a way for this file to
rot without anyone noticing. The branch folds away at -O3 either way.
*/
constexpr bool kSanitizedBuild{
#if defined(_GLIBCXX_DEBUG) || defined(__SANITIZE_ADDRESS__) || \
    defined(__SANITIZE_THREAD__)
    true
#else
    false
#endif
};

template <bool kInstrument>
void flash1Bench(benchmark::State& state, std::string_view scenario) {
  if (kSanitizedBuild) {
    // exchange_bench is not EXCLUDE_FROM_ALL and inherits `debug_options` from
    // `engine` in Debug. Without this guard a Debug run produces a beautifully
    // shaped histogram that is off by ~50x, and nothing about it looks wrong.
    state.SkipWithError(
        "built with sanitizers/_GLIBCXX_DEBUG — latency numbers here would be "
        "meaningless; build with the release preset");
    return;
  }

  const LoadedWorkload& workload{cachedWorkload(scenario)};
  if (!workload.error.empty()) {
    state.SkipWithError(workload.error.c_str());
    return;
  }
  const std::vector<WorkloadRecord>& records{workload.records};
  if (records.empty()) {
    state.SkipWithError("workload is empty");
    return;
  }

  [[maybe_unused]] const TimerCalibration* calibration{nullptr};
  if constexpr (kInstrument) {
    calibration = &calibratedTimer();
    g_timer_used = true;
  }

  HistSet histograms{};
  Counters counters{};
  double replay_ns{0.0};
  std::optional<OrderBook> book{};

  /*
  Pattern B (BenchSupport.hpp): the book is move-assigned inside the paused
  region so the previous one's destructor is paused too. Registration pins
  ->Iterations(1), so this batch runs exactly once per repetition — which is
  what makes the histogram unambiguous. Google Benchmark's default ramp calls
  the body several times with growing iteration counts and keeps only the last,
  and samples from the discarded trials would silently blend into the published
  distribution.
  */
  while (state.KeepRunningBatch(static_cast<int64_t>(records.size()))) {
    state.PauseTiming();
    book.emplace(Symbol{"FLASH1"});
    for (LatencyHistogram& histogram : histograms) histogram.reset();
    state.ResumeTiming();

    const auto begin{std::chrono::steady_clock::now()};
    counters = replayStream<kInstrument>(*book, records, histograms);
    const auto end{std::chrono::steady_clock::now()};
    replay_ns = std::chrono::duration<double, std::nano>{end - begin}.count();
  }

  state.SetItemsProcessed(state.iterations());

  HistRun& run{registryEntry(state.name())};
  run.scenario = scenario;
  run.instrumented = kInstrument;
  run.messages = records.size();
  ++run.repetitions;
  run.total_replay_ns += replay_ns;
  run.total_messages += records.size();
  run.counters.fills += counters.fills;
  run.counters.cancel_rejects += counters.cancel_rejects;
  run.counters.modify_rejects += counters.modify_rejects;
  run.counters.timer_anomalies += counters.timer_anomalies;
  for (std::size_t i{0}; i < kOpKindCount; ++i)
    run.by_kind[i].merge(histograms[i]);

  LatencyHistogram merged{};
  for (const LatencyHistogram& histogram : histograms) merged.merge(histogram);
  if constexpr (kInstrument) {
    // Named for the unit they actually carry: without a trustworthy tick rate
    // these are ticks, and a column headed p50_ns holding ticks is worse than
    // no column at all.
    const std::string_view unit{calibration->inNanoseconds() ? "ns" : "ticks"};
    state.counters[std::format("p50_{}", unit)] =
        calibration->toNs(merged.quantileTicks(0.50));
    state.counters[std::format("p99_{}", unit)] =
        calibration->toNs(merged.quantileTicks(0.99));
    state.counters[std::format("p999_{}", unit)] =
        calibration->toNs(merged.quantileTicks(0.999));
    state.counters[std::format("max_{}", unit)] =
        calibration->toNs(merged.maxTicks());
  }

  /*
  The correctness gate, in place of a hash — correctness itself stays with
  `scripts/run_flash1.sh audit`. What is checked here is that the stream really
  replayed: the generator plants ~2% duplicate cancels and stale modifies, so
  zero rejects means the messages never reached the book, and a flood means the
  run found a fast failure path. Same reasoning as net_workload_bench.
  */
  if (counters.fills == 0) {
    state.SkipWithError("no fills — the flash1 stream never crossed");
  } else if (counters.cancel_rejects == 0) {
    state.SkipWithError(
        "no rejected cancels — the planted duplicate-cancel path was never "
        "exercised, so this did not replay the flash1 stream");
  } else if (counters.cancel_rejects > records.size() / 10) {
    state.SkipWithError(
        "more than 10% of cancels rejected — this is a failure path, not a "
        "result");
  } else if (counters.timer_anomalies > records.size() / 10000) {
    state.SkipWithError(
        "backwards TSC deltas — the thread migrated across cores with "
        "unsynchronised TSCs; these times are not trustworthy");
  }

  std::string label{std::format(
      "scenario={} msgs={} fills={} cancel_rejects={} modify_rejects={}",
      scenario, records.size(), counters.fills, counters.cancel_rejects,
      counters.modify_rejects)};
  if constexpr (kInstrument) {
    label += std::format(
        " timer_floor={:.1f}{} anomalies={}",
        calibration->toNs(calibration->overhead.quantileTicks(0.5)),
        calibration->inNanoseconds() ? "ns" : "tk", counters.timer_anomalies);
    if (!calibration->inNanoseconds())
      label += " WARNING:non-invariant-TSC-x-axis-is-ticks";
  }
  label += std::format(" harness={}", resolveHarnessDir().string());
  state.SetLabel(label);
}

// Registered here rather than in main() so the benchmarks live next to their
// definition like the rest of the suite; the options they read are filled in
// before RunSpecifiedBenchmarks(). Twins are registered adjacently so the
// console prints the instrumented row directly above its uninstrumented
// control. Registration is unconditional — a missing external/ is reported by
// the benchmark itself, so `--benchmark_filter='^BM_Flash1_'` never silently
// matches nothing.
//
// No SetComplexityN anywhere: the five scenarios are categories, not a size
// sweep, and a complexity fit over them would be meaningless.
[[maybe_unused]] const int kRegisterFlash1Benchmarks{[] {
  for (const std::string_view scenario : kScenarios) {
    benchmark::RegisterBenchmark(std::format("BM_Flash1_Replay/{}", scenario),
                                 [scenario](benchmark::State& state) {
                                   flash1Bench<true>(state, scenario);
                                 })
        ->Iterations(1)
        ->Unit(benchmark::kNanosecond);
    benchmark::RegisterBenchmark(
        std::format("BM_Flash1_Replay_NoTiming/{}", scenario),
        [scenario](benchmark::State& state) {
          flash1Bench<false>(state, scenario);
        })
        ->Iterations(1)
        ->Unit(benchmark::kNanosecond);
  }
  return 0;
}()};

// --- sidecar JSON -----------------------------------------------------------
//
// Hand-rolled, as in bench/net/net_workload_bench.cpp: this target links no
// JSON library and is not going to grow one for eight fields.

std::string jsonEscape(std::string_view text) {
  std::string escaped{};
  escaped.reserve(text.size() + 8);
  for (const char c : text) {
    switch (c) {
      case '"':
        escaped += "\\\"";
        break;
      case '\\':
        escaped += "\\\\";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20)
          escaped += std::format("\\u{:04x}", static_cast<unsigned>(c));
        else
          escaped += c;
    }
  }
  return escaped;
}

// A nan or inf here would make the whole document unparseable.
std::string jsonNumber(double value) {
  if (!std::isfinite(value)) return "null";
  return std::format("{:.6g}", value);
}

void writeHistogram(std::string& out, const LatencyHistogram& histogram,
                    const TimerCalibration& calibration) {
  const auto [first, last]{histogram.occupiedRange()};

  out += std::format(
      "        \"count\": {}, \"overflow\": {},\n"
      "        \"min\": {}, \"max\": {}, \"mean\": {},\n"
      "        \"p50\": {}, \"p90\": {}, \"p99\": {}, \"p999\": {},\n",
      histogram.total(), histogram.overflow(),
      jsonNumber(calibration.toNs(histogram.minTicks())),
      jsonNumber(calibration.toNs(histogram.maxTicks())),
      jsonNumber(calibration.toNs(1) * histogram.meanTicks()),
      jsonNumber(calibration.toNs(histogram.quantileTicks(0.50))),
      jsonNumber(calibration.toNs(histogram.quantileTicks(0.90))),
      jsonNumber(calibration.toNs(histogram.quantileTicks(0.99))),
      jsonNumber(calibration.toNs(histogram.quantileTicks(0.999))));

  // Only the occupied contiguous range, as N+1 edges and N counts — exactly
  // what matplotlib's ax.stairs() consumes, with no reconstruction needed.
  // Interior zeros are kept: a gap between two modes is data.
  out += "        \"edges\": [";
  for (std::size_t i{first}; i < last; ++i) {
    out += std::format("{}{}", i == first ? "" : ", ",
                       jsonNumber(calibration.toNs(bucketRange(i).first)));
  }
  if (first != last)
    out += std::format(
        ", {}", jsonNumber(calibration.toNs(bucketRange(last - 1).second)));
  out += "],\n        \"counts\": [";
  for (std::size_t i{first}; i < last; ++i)
    out += std::format("{}{}", i == first ? "" : ", ", histogram.count(i));
  out += "]\n";
}

}  // namespace

namespace Exchange::Bench {

Flash1Options& flash1Options() {
  static Flash1Options options{};
  return options;
}

bool writeHistJson(const std::string& path) {
  const Flash1Options& options{flash1Options()};
  std::string out{};
  out += "{\n  \"hist_schema\": 1,\n";

  if (g_timer_used) {
    const TimerCalibration& calibration{calibratedTimer()};
    const LatencyHistogram& overhead{calibration.overhead};
    out += std::format(
        "  \"timer\": {{\n"
        "    \"source\": \"{}\",\n"
        "    \"invariant_tsc\": {},\n"
        "    \"invariant_evidence\": \"{}\",\n"
        "    \"clocksource\": \"{}\",\n"
        "    \"ns_per_tick\": {},\n"
        "    \"calibration_rounds\": {},\n"
        "    \"calibration_spread_ppm\": {},\n"
        "    \"resolution\": {},\n"
        "    \"units\": \"{}\",\n"
        "    \"overhead\": {{\"count\": {}, \"min\": {}, \"p50\": {}, "
        "\"p99\": {}, \"max\": {}}}\n"
        "  }},\n",
        jsonEscape(calibration.source),
        calibration.invariant_tsc ? "true" : "false",
        jsonEscape(calibration.invariant_evidence),
        jsonEscape(calibration.clocksource),
        calibration.inNanoseconds() ? jsonNumber(calibration.ns_per_tick)
                                    : std::string{"null"},
        calibration.calibration_rounds,
        jsonNumber(calibration.calibration_spread_ppm),
        jsonNumber(calibration.toNs(calibration.resolution_ticks)),
        calibration.inNanoseconds() ? "ns" : "ticks", overhead.total(),
        jsonNumber(calibration.toNs(overhead.minTicks())),
        jsonNumber(calibration.toNs(overhead.quantileTicks(0.50))),
        jsonNumber(calibration.toNs(overhead.quantileTicks(0.99))),
        jsonNumber(calibration.toNs(overhead.maxTicks())));
  } else {
    out += "  \"timer\": null,\n";
  }

  out += std::format(
      "  \"histogram\": {{\"sub_bucket_bits\": {}, \"max_relative_error\": "
      "{}}},\n"
      "  \"workload\": {{\"seed\": {}, \"count\": {}, \"harness_dir\": "
      "\"{}\"}},\n",
      kSubBucketBits, jsonNumber(1.0 / static_cast<double>(kSubBucketCount)),
      options.seed == 0 ? kCanonicalSeed : options.seed,
      options.count == 0 ? kCanonicalCount : options.count,
      jsonEscape(resolveHarnessDir().string()));

  // A run that never timed anything still needs a calibration to format its
  // (empty) numbers against; a default-constructed one reports ticks 1:1.
  static const TimerCalibration kIdentity{};
  const TimerCalibration& calibration{g_timer_used ? calibratedTimer()
                                                   : kIdentity};

  out += "  \"runs\": [";
  bool first_run{true};
  for (const HistRun& run : histRegistry()) {
    out += first_run ? "\n" : ",\n";
    first_run = false;
    out += std::format(
        "    {{\n"
        "      \"benchmark\": \"{}\",\n"
        "      \"scenario\": \"{}\",\n"
        "      \"instrumented\": {},\n"
        "      \"repetitions\": {},\n"
        "      \"messages\": {},\n"
        "      \"ns_per_op\": {},\n"
        "      \"counters\": {{\"fills\": {}, \"cancel_rejects\": {}, "
        "\"modify_rejects\": {}, \"timer_anomalies\": {}}}",
        jsonEscape(run.benchmark), jsonEscape(run.scenario),
        run.instrumented ? "true" : "false", run.repetitions, run.messages,
        run.total_messages == 0
            ? std::string{"null"}
            : jsonNumber(run.total_replay_ns /
                         static_cast<double>(run.total_messages)),
        run.counters.fills, run.counters.cancel_rejects,
        run.counters.modify_rejects, run.counters.timer_anomalies);

    if (!run.instrumented) {
      // The twin exists only to price the instrument; it has no distribution.
      out += ",\n      \"ops\": {}\n    }";
      continue;
    }
    out += ",\n      \"ops\": {\n";
    for (std::size_t i{0}; i < kOpKindCount; ++i) {
      out += std::format("      \"{}\": {{\n", kOpKindNames[i]);
      writeHistogram(out, run.by_kind[i], calibration);
      out += i + 1 == kOpKindCount ? "      }\n" : "      },\n";
    }
    out += "      }\n    }";
  }
  out += first_run ? "]\n}\n" : "\n  ]\n}\n";

  std::FILE* file{std::fopen(path.c_str(), "w")};
  if (file == nullptr) return false;
  const std::size_t written{std::fwrite(out.data(), 1, out.size(), file)};
  return std::fclose(file) == 0 && written == out.size();
}

}  // namespace Exchange::Bench
