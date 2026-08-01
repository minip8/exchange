# Benchmarking

Four things get measured, and they answer different questions:

| Suite | Question | Binary / script |
|---|---|---|
| **Google Benchmark** | how long does one book/engine operation take? | `exchange_bench` |
| **flash1 harness** | is the engine *correct*, and how many messages/s under five volatility regimes? | `scripts/run_flash1.sh` |
| **Network workload** | what does the networking layer cost, and where is the knee? | `net_workload_bench` |
| **Loopback micro-probe** | ring egress vs `asio::post` — what does one wake carry? | `net_loopback_bench` |

Plus two pipelines (`scripts/bench_pipeline.sh`, `scripts/net_bench_pipeline.sh`)
that run, record and plot.

> **Always benchmark the Release config.** Every bench target links `engine`,
> which propagates `debug_options` PUBLIC — so a bench built in `build/debug/`
> carries ASan/UBSan/LSan and `_GLIBCXX_DEBUG` even though it also sets
> `-O3 -march=native` on itself. Optimized *and* sanitized numbers are
> meaningless. See [BUILDING.md](BUILDING.md#debug-vs-release-which-to-use-when).

- [Google Benchmark suite](#google-benchmark-suite)
- [The flash1 harness](#the-flash1-harness)
- [The engine benchmark pipeline](#the-engine-benchmark-pipeline)
- [Network workload benchmark](#network-workload-benchmark)
- [Loopback latency micro-probe](#loopback-latency-micro-probe)
- [The network benchmark pipeline](#the-network-benchmark-pipeline)
- [Reading the results](#reading-the-results)

## Google Benchmark suite

Microbenchmarks over `OrderBook`, `MatchingEngine` and the SPSC ring.
googlebenchmark is pinned via FetchContent by `bench/google/CMakeLists.txt`.

```bash
cmake --build --preset bench
./build/release/bench/google/exchange_bench

# one benchmark, or a family, by regex
./build/release/bench/google/exchange_bench --benchmark_filter='BM_AddOrder.*'

# machine-readable, for regression gating
./build/release/bench/google/exchange_bench --benchmark_format=json > bench_results.json
```

What is in there: `BM_AddOrder_*` (empty book, at depth, matching top-of-book,
sweeping levels), `BM_RemoveOrder_AtDepth`, `BM_MixedWorkload_SteadyState`,
the `BM_GetOrderBook_By{Id,OrderId,Symbol}` lookups, `BM_Engine_AddOrder_Routed`,
`BM_Engine_MultiBook_SteadyState`, and `BM_Ring{PushPop,Batch,PingPong}`.

## The flash1 harness

[flash1-dev/matching-engine-benchmark](https://github.com/flash1-dev/matching-engine-benchmark),
integrated as an external **conformance + throughput** benchmark. This is the
real correctness gate for the engine. The adapter (`bench/flash1/adapter.cpp`)
wraps `OrderBook` directly; harness-supplied ids pass through via the explicit-id
`Order` constructor.

Needs `libboost-dev`, g++ 14+, python3 3.8+. The harness is cloned pinned into a
gitignored `external/`.

```bash
scripts/fetch_harness.sh                 # one-time: clone + build into external/
scripts/run_flash1.sh build              # build adapter.so into build/release/
scripts/run_flash1.sh audit  [scenario]  # correctness + book state audit
scripts/run_flash1.sh perf   [scenario]  # timed run (verifies the hash too)
scripts/run_flash1.sh conformance        # pre-flight conformance check
scripts/run_flash1.sh explain <scenario> # dump canonical output, localize first divergence
scripts/run_flash1.sh challenge          # 10 perf + 1 audit per scenario, worst-case
```

Scenarios: `static | normal | swing-25 | swing-40 | flash-crash`.

`run_flash1.sh build` builds `flash1_adapter` via the `release` preset, so the
adapter path is deterministic and perf numbers never depend on how some tree
happens to be configured. The adapter is sanitizer-free in *any* config because
it compiles `OrderBook.cpp` directly instead of linking `engine`.

**When a hash mismatches, reach for `explain <scenario>`** — it dumps the
canonical output and localizes the first divergence.

**Every engine change gets `scripts/run_flash1.sh audit` on all five scenarios
before merge.** `Status: PASS` with a matching hash and `Verdict: VALID` is the
gate. (Phases 0–6 additionally must not touch `OrderBook.hpp`, `Order.hpp`,
`PriceLevel.hpp` or `Symbol.hpp` — the harness compiles against them.)

## The engine benchmark pipeline

`scripts/bench_pipeline.sh` runs the Google Benchmark suite (Release) plus one
flash1 `perf` rep per scenario, writes a timestamped + git-SHA-tagged record JSON
to gitignored `bench/results/<shortsha>/`, and renders PNGs.

**Each measured commit owns a directory.** Its record and its snapshot plots sit
together in `bench/results/<shortsha>/`, so one commit's data can be read on its
own; only the cross-commit trend charts live at the root in
`bench/results/plots/`. A commit measured more than once keeps every record —
they differ by the timestamp in the filename — and the newest drives that
commit's plots.

```bash
scripts/bench_pipeline.sh                 # quick: 5 flash1 perf reps/scenario
scripts/bench_pipeline.sh --full          # 10 reps/scenario, median recorded
scripts/bench_pipeline.sh --skip-flash1   # Google Benchmark suite only
scripts/bench_pipeline.sh --skip-hist     # skip the per-op latency distribution
scripts/bench_pipeline.sh --net           # also run the loopback latency bench
scripts/bench_pipeline.sh --plot-only     # re-render plots from existing records
```

`--full` is challenge-style scoring, matching `run_challenge.py`'s 10 reps.
Plots: `bench/results/<shortsha>/plots/{flash1,gb,flash1_latency}.png` for the
commit measured, and `bench/results/plots/{flash1_history,gb_history}.png` for
the trend across all of them. `--plot-only` re-renders every commit's snapshots
from its own newest record, then the trend.

Plotting needs `uv`; Python deps are declared inline in `scripts/plot_bench.py`
(PEP 723). `--net` output is deliberately *not* folded into the record JSON — a
latency distribution across pipeline depths and two egress implementations does
not fit the one-number-per-scenario shape the plots are built around.

`exchange_bench` is invoked **twice**, and this is not an accident: the default
run carries `--benchmark_filter='-^BM_Flash1_'` (the leading `-` is Google
Benchmark's negative filter) and a second run does only the flash1 replay. They
are different processes so the microbenchmark numbers the pipeline trends are
untouched by a 2M-message stream replay sharing their address space.

## The flash1 per-operation latency distribution

Everything else in the Google Benchmark suite reports a mean over a synthetic
fixture. `BM_Flash1_Replay/<scenario>` (`bench/google/bench_flash1_replay.cpp`)
instead replays the *actual* flash1 order stream through one `OrderBook`, timing
every message individually, and reports the **distribution**: how long one
operation took, and how often. That is what separates "the median add is 60 ns"
from "one add in a thousand sweeps 40 levels and costs 9 µs".

```bash
cmake --build --preset bench
./build/release/bench/google/exchange_bench \
  --benchmark_filter='^BM_Flash1_' \
  --harness-dir "$PWD/external/matching-engine-benchmark" \
  --hist-json /tmp/hist.json
```

Opt-in, and it self-skips with a clear message when `external/` is absent, so
the flags above are the whole interface. `--hist-count` / `--hist-seed` pick a
different generator stream; `bench_main.cpp` owns these flags, which is why this
binary no longer links `benchmark_main`.

Replay semantics are **byte-identical to `bench/flash1/adapter.cpp`** — the same
price sign-flip, the same explicit-id `Order` constructor, the same IOC residual
pull, modify as remove + re-add. If the two diverge these numbers stop describing
what the conformance harness measures.

Four things are load-bearing:

- **This deliberately breaks rule 2 of `BenchSupport.hpp`** (no clock reads in a
  timed region). Here the per-operation time *is* the measurement. The tax is
  therefore measured rather than assumed, twice over: `TscTimer.hpp` publishes
  the distribution of an empty `rdtsc` start/stop pair (the "timer floor" line on
  the chart — everything left of it is instrument, not engine), and every
  benchmark has a `_NoTiming` twin running the identical replay with the timing
  compiled out. **The ns/op this benchmark reports is instrumented**; the twin
  beside it is the real per-operation cost. Expect a large gap (~+90% on
  `normal`): the fences also forbid the cross-operation overlap the twin enjoys,
  so the twin is a lower bound, not a matched control.
- **Registered with `->Iterations(1)`.** Google Benchmark's default ramp calls the
  body several times with growing iteration counts and keeps only the last;
  samples from those discarded trials would silently blend into the published
  distribution. An explicit iteration count short-circuits the ramp
  (`benchmark_runner.cc:517-521`), so one repetition is exactly one stream replay.
- **The TSC does not advance one count at a time.** On WSL2 here it steps ~39
  counts, so the instrument's real resolution is ~10 ns, not 0.26 ns — reported
  as `timer.resolution` and used by `plot_bench.py` to pick display bins. Without
  it the low end of the histogram is a picket fence of alternating occupied and
  empty buckets. Note also that the CPUID invariant-TSC bit is *masked* under
  Hyper-V, so `TscTimer.hpp` accepts kernel corroboration (`constant_tsc` +
  `nonstop_tsc`, or `tsc_known_freq`) as evidence and records which it used in
  `timer.invariant_evidence`. With no evidence at all it publishes **ticks**, not
  nanoseconds, and says so on the chart — a frequency-scaled TSC gives a
  perfectly plausible shape with a false scale.
- **The gate is the reject counts, not a hash.** Correctness stays with
  `scripts/run_flash1.sh audit`. The generator plants ~2% duplicate cancels and
  stale modifies, so zero rejected cancels means the stream never reached the
  book, and a flood means the run found a fast failure path. Either fails the
  benchmark via `SkipWithError`, as does a backwards TSC delta (a thread
  migration across cores with unsynchronised TSCs).

Reading `flash1_latency.png`: one panel per scenario, one colour per message
kind, log on both axes — the mode is ~1e6 samples and the tail is ~1e0, so on a
linear y the tail is a flat line on the axis. The `static` panel is the
interesting one: with almost nothing crossing, the book grows without bound and
`cancel` develops a second mode two decades right of the first, which is the
vector book's linear level scan made visible.

Sidecar JSON (`--hist-json`) carries full-resolution buckets as `edges` + `counts`
per kind per scenario, plus exact per-kind percentiles; `plot_bench.py --hist-json`
embeds it in the record under `flash1_latency`. That key is additive — records
written before it exist simply lack it, which is why `SCHEMA_VERSION` did not
move.

## Network workload benchmark

`net_workload_bench` replays the **flash1 order stream** through the networking
layer, so the network numbers sit on the same axis as the engine numbers: same
five volatility regimes, same messages, same prices, no second synthetic workload
to keep honest.

```bash
cmake --build --preset net-bench
./build/release/bench/net/net_workload_bench --help
```

```
--mode gateway|tcp|both   what to measure (default both)
--scenario NAME           repeatable; default all five flash1 scenarios
--count N                 messages requested of the generator (default 1000000)
--seed N                  workload seed (default 23)
--harness-dir PATH        where the flash1 harness lives
--rates A,B,C             offered rates for tcp mode; 0 means saturation
                          (default 250000,500000,1000000,0)
--clients N               connections sharing the stream (default 1)
--scaling A,B,C           also run saturation at each client count
--io-threads N            server I/O threads (default 1)
--egress ring|post        server egress implementation (default ring)
--spin-us N               matching-thread idle spin (default 0)
--warmup-fraction F       leading share of the stream left untimed (0.05)
--subscribe-md            also subscribe each client to L2 depth
--json PATH               write machine-readable results
```

**Two modes.** `gateway` drives `Command`s straight through `MatchingLoop` with
no sockets; the gap between it and the flash1 harness's own M msgs/s is the price
of the gateway's ownership tracking, `leaves` and market data. `tcp` is
**open-loop**: sends are paced at a target offered rate and the latency clock
starts at the *scheduled* send time, not the actual one, so a driver that falls
behind puts its own backlog into the number. That is the point — a closed-loop
measurement cannot show a knee.

The order stream lives on disk with the harness (`./generator` writes
`orders_<scenario>_s<seed>_n<count>.bin`); the bench only shells out to generate
one when it is missing. Building needs no `external/`; running does.

### The gate

There is no correctness hash here — correctness stays with
`run_flash1.sh audit`. This gates on **reject counts** instead. The generator
plants ~2% duplicate cancels and stale modifies, so:

- **zero `UnknownOrder` rejects means the stream did not really replay**, and
- a flood of them means the run found a fast failure path.

Any coid mismatch on an ack is counted as `desync` and **fails the run**.

## Loopback latency micro-probe

`net_loopback_bench` reports p50/p99/p99.9 for the gateway alone and for a full
TCP round trip, across three pipeline depths and both egress implementations.

```bash
cmake --build --preset loopback-bench
./build/release/bench/loopback/net_loopback_bench [--iterations N] [--spin-us N] [--json PATH]
```

Measured result: the lock-free egress and `asio::post` are indistinguishable at
depth 1, reach parity around depth 8, and the ring pulls ~8–10% ahead on median
and throughput by depth 32 where one eventfd wake carries ~40 events. At this
project's real load the ring buys nothing measurable — which is what the design
predicted, and what the benchmark exists to check rather than assume.
`--json PATH` writes the same numbers machine-readably; stdout is unchanged
either way.

## The network benchmark pipeline

`scripts/net_bench_pipeline.sh` is the `bench_pipeline.sh` equivalent for the
network: run, record to `bench/results/net/<shortsha>/run_<ts>_<sha>.json`, plot
to that same directory. Same per-commit layout as the engine pipeline.

```bash
scripts/net_bench_pipeline.sh                  # quick: 200k-order stream, all five scenarios
scripts/net_bench_pipeline.sh --full           # the canonical 1M-order stream, wider rate sweep
scripts/net_bench_pipeline.sh --scenario normal   # repeatable
scripts/net_bench_pipeline.sh --skip-loopback  # skip the egress micro-probe
scripts/net_bench_pipeline.sh --plot-only      # re-render from existing records
```

Quick sweeps `100000,250000,500000,1000000,0` (0 = saturation) over 200k orders;
`--full` uses 1M orders and adds a 2 M/s point. Client scaling is `1,2,4`. The
pipeline runs with `--spin-us 200` so the numbers measure the transport rather
than the idle-wake policy. Plots:
`bench/results/net/<shortsha>/plots/{net_latency_vs_rate,net_throughput,net_scaling}.png`,
plus `bench/results/net/plots/net_history.png` for the trend.

## Reading the results

### The flash1 score is the worst case, not the average

Baseline (WSL2, July 2026): normal 1.91 M/s, static 1.12, swing-25 0.16,
swing-40 0.092, flash-crash 0.059 — so the score is **0.059 M/s**.

**These baseline numbers are known to be low and are deliberately left as-is.**
They are an early-history snapshot, not a current measurement. As of July 2026
flash-crash measures ~4.2 M/s, roughly 70× that figure, and the
volatile-scenario collapse the baseline shows (attributed to the vector book's
linear level scans) no longer reproduces.

So **beating the baseline by one or two orders of magnitude is expected, not a
red flag** — in particular it does not mean a change has broken the harness into
a fast failure path. Check that the usual way:
`scripts/run_flash1.sh audit <scenario>` → `Status: PASS` with a matching hash
and `Verdict: VALID`. That is the real correctness gate.

To attribute a throughput change, **compare against a build of the parent
commit**, not against these figures. `BASELINE_MPS` in `scripts/plot_bench.py`
draws the reference lines and should be treated the same way: a floor marker,
not a target. Keep it in sync with the numbers above if you change either.

### The network plots

`net_latency_vs_rate.png` is the one that matters — p50/p99/p99.9 against offered
load, with saturation marked. **The p50 and p99 curves are the signal.** The
p99.9 tail is noisy on a loaded desktop (three hot threads with
`--spin-us 200`), so a non-monotonic p99.9 between two paced points is scheduler
noise, not a knee.

### Records

`bench/results/` is gitignored. Records are timestamped and git-SHA-tagged, and
both pipelines mark a run `dirty` if the working tree was not clean — so a
record always says what it was measuring. Each lives in `<shortsha>/` beside the
plots rendered from it; a directory is treated as a commit's iff its name is
exactly seven hex characters, which is what keeps `plots/`, `net/` and
`archive_broken_suite/` out of the history walk as siblings.

Plots reach git only through `scripts/publish_bench_plots.sh`, which copies the
per-commit PNGs the README embeds into tracked `docs/bench/<shortsha>/` and
prints the matching markdown. Publishing is a deliberate step, so a scratch run
never lands in the repo just because it was measured last.
