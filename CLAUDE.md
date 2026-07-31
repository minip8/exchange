# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

A limit-order-book matching engine written in **C++26**, plus a networked front end around it.

The engine itself (`libengine.a`) is unchanged in character: single-threaded, in-memory, no persistence, and it remains the thing the flash1 conformance harness measures. Everything the network needs — order state, `leaves`, the book directory, ownership, L2 aggregates — is **gateway-owned**, which is what keeps the benchmarked core pure.

The networking layer (`src/net/`, `include/net/`) is I/O threads feeding one matching thread through lock-free SPSC rings, with two protocols over one core: a custom binary TCP protocol for algo clients and WebSocket/JSON for a browser GUI. See the "Networking" section below.

## Build & Run

The build uses **CMake + Ninja** with **g++**, driven entirely through `CMakePresets.json`. C++26 with `-fexperimental-library`-level features (`std::expected`, `std::print`, `std::function_ref`, explicit object parameters / "deducing `this`").

Three **single-config** trees, one per config, each pinned by a preset: `build/debug/` (the development default — ASan/UBSan/LSan + `_GLIBCXX_DEBUG`), `build/tsan/` (ThreadSanitizer; see Networking) and `build/release/` (optimized, sanitizer-free, the only config benchmarks may run in). Which sanitizers the Debug config carries is selected by `EXCHANGE_SANITIZER` (`address` | `thread` | `none`). Because the trees are single-config, artifacts land directly in the tree with **no per-config subdirectory**, and `--config` is never used — passing it to a single-config tree is silently ignored, which is exactly the failure this layout exists to prevent. Always address a tree by its preset, never by an ad-hoc `-B` path.

```bash
# Configure (once per tree; re-running is idempotent and cheap)
cmake --preset debug
cmake --preset release

# Build
cmake --build --preset debug      # everything, Debug
cmake --build --preset release    # everything, Release
cmake --build --preset tsan       # everything, ThreadSanitizer
cmake --build --preset bench      # just exchange_bench (Release)
cmake --build --preset flash1     # just the flash1 adapter (Release)
cmake --build --preset server     # just exchange_server (Release)
cmake --build --preset smoke      # just net_smoke (Debug)
cmake --build --preset loopback-bench   # network latency bench (Release)

# Run the smoke-test executable
./build/debug/app

# Run benchmarks
./build/release/bench/google/exchange_bench

# Run a single benchmark by name (regex)
./build/release/bench/google/exchange_bench --benchmark_filter='BM_AddOrder.*'

# JSON output for regression gating (see bench/google/CMakeLists.txt for the intended CI flow)
./build/release/bench/google/exchange_bench --benchmark_format=json > bench_results.json
```

`cmake --preset` resolves `CMakePresets.json` from the **current working directory**, so scripts that may be invoked from anywhere must `cd` to the repo root first (both `scripts/run_flash1.sh` and `scripts/bench_pipeline.sh` do).

There is **no unit-test suite** and none should be added — correctness is exercised via `src/main.cpp`, `src/net/smoke/net_smoke.cpp`, the benchmarks, and the flash1 conformance harness (the real gate for the engine; see below). `src/main.cpp` is a hand-rolled smoke test: it checks with a local `check()` helper rather than `assert`, because Release defines `NDEBUG` and would compile the assertions away. It prints each failure and exits non-zero, so it is usable as a CI step.

`bench/CMakeLists.txt` is a pure dispatcher over two peer suites, each owning its own `CMakeLists.txt`: `bench/google/` (the Google Benchmark microbenchmarks — target `exchange_bench`, which also pins the googlebenchmark FetchContent dependency) and `bench/flash1/` (the external conformance harness adapter). Benchmark results and plots land in the gitignored `bench/results/`.

**Always benchmark the Release config.** `exchange_bench` sets `-O3 -march=native` on itself in every config, but it links `engine`, which propagates `debug_options` as a PUBLIC dependency — so a bench binary built in `build/debug/` is still built with ASan/UBSan/LSan and `_GLIBCXX_DEBUG`. Optimized *and* sanitized numbers are meaningless. Keep Debug for development so the sanitizers catch bugs.

**Never put a `PauseTiming()` in a `for (auto _ : state)` body.** `bench/google/BenchSupport.hpp` documents the two fixture patterns that replace it and why. The short version: pause/resume costs ~640 ns, which swamps operations that take tens of ns, and — the subtler half — a fixture declared inside the loop body is *destroyed* after `ResumeTiming()`, so the destructor lands in the timed region. That defect once made `BM_Engine_AddOrder_Routed`, `BM_Engine_RemoveOrder` and `BM_Engine_AddOrderBook` report the same ~1 ms at 8192 books, because all three were measuring `~SeededEngine` rather than the operation named in the benchmark. Use **Pattern A** (self-inverse pair — run the operation and its inverse so the fixture is unchanged, needing no timer manipulation at all) or **Pattern B** (`KeepRunningBatch` with the rebuild done by move-assigning into a `std::optional` inside the paused region, so the previous fixture's teardown is paused too). No clock reads and no RNG draws inside the timed region either — times come from a monotonic counter (`seqTime`) and randomness is pre-generated into a script.

### flash1 benchmark harness

The [flash1-dev/matching-engine-benchmark](https://github.com/flash1-dev/matching-engine-benchmark) harness is integrated as an external conformance + throughput benchmark. The adapter lives in `bench/flash1/adapter.cpp` and wraps `OrderBook` directly (harness-supplied ids pass through via the explicit-id `Order` constructor). The harness itself is cloned pinned into a gitignored `external/`.

```bash
scripts/fetch_harness.sh                 # one-time: clone + build the harness into external/
scripts/run_flash1.sh build              # build adapter.so into build/release/
scripts/run_flash1.sh audit  [scenario]  # correctness + book state audit
scripts/run_flash1.sh perf   [scenario]  # timed run (also verifies the hash)
scripts/run_flash1.sh challenge          # 10 perf + 1 audit per scenario, worst-case result
```

Also available: `conformance` (pre-flight check) and `explain <scenario>`, which dumps the canonical output and localizes the first divergence — the tool to reach for when a hash mismatches.

Scenarios: `static | normal | swing-25 | swing-40 | flash-crash`. Requires `libboost-dev` (installed system-wide); the scripts hardcode no Boost paths.

`run_flash1.sh build` builds `flash1_adapter` via the `release` preset, so the adapter path is deterministic and perf numbers never depend on how some tree happens to be configured. The adapter is additionally sanitizer-free in *any* config: it compiles `OrderBook.cpp` directly rather than linking `engine`, so `debug_options` never reaches it.

### Benchmark pipeline (run + record + plot)

`scripts/bench_pipeline.sh` runs the Google Benchmark suite (Release) plus one flash1 `perf` rep per scenario, writes a timestamped + git-SHA-tagged record JSON to gitignored `bench/results/`, and renders PNGs (latest snapshot + trend over history) to `bench/results/plots/`. Flags: `--full` (10 flash1 reps/scenario — challenge-style scoring), `--skip-flash1`, `--skip-hist`, `--plot-only`. Plotting needs `uv`; Python deps are declared inline in `scripts/plot_bench.py` (PEP 723). The flash1 baseline used for reference lines is hardcoded in `plot_bench.py` (`BASELINE_MPS`) — keep it in sync with the numbers below.

`bench_pipeline.sh` invokes `exchange_bench` **twice**: the default run excludes the flash1 replay benchmarks (`--benchmark_filter='-^BM_Flash1_'` — the leading `-` is Google Benchmark's negative filter), and a second filtered run does only those, writing the per-op latency sidecar. Different processes, so a 2M-message stream replay cannot perturb the microbenchmarks being trended. See "flash1 per-operation latency distribution" below.

Google Benchmark runs at **1 repetition** in both `bench_pipeline.sh` and `bench_backfill.sh`, in every mode — neither script exposes a knob for it. Records therefore carry a single `iteration` row per benchmark and no aggregates; `plot_bench.py` handles that (its `gb_entries` falls back to `iteration` rows, and `gb_stddev` returns `None`, so the trend chart draws no spread band).

Note that one repetition cannot separate a real few-percent change from machine drift, and reading a single-rep cross-run delta as signal is a mistake this repo has already made once — treat small deltas between records as noise unless a repeated manual run confirms them (`exchange_bench --benchmark_repetitions=N` directly).

`scripts/bench_backfill.sh` re-measures *historical* commits with the current bench sources: one git worktree per commit, this tree's `bench/google/` copied in, recorded against the commit's own sha and commit date. `bench_pipeline.sh` cannot do this — it always records HEAD at wall-clock now. Use it after changing a benchmark, because records made by two different versions of a benchmark are not comparable and will otherwise put a fake cliff in the trend chart. Backfilling is bounded below by the bench sources' own dependencies: they need `Symbol` (`a925d17`) and `net_protocol` (`560595f`), so older commits cannot be measured at all.

**The score is the worst-case scenario, not the average.** Baseline (WSL2, July 2026): normal 1.91 M/s, static 1.12, swing-25 0.16, swing-40 0.092, flash-crash 0.059 — so 0.059 M/s. The collapse on volatile scenarios is attributed to the vector book's linear level scans.

**These baseline numbers are known to be low, and are deliberately left as-is.** They are an early-history snapshot, not a current measurement — as of July 2026 flash-crash measures ~4.2 M/s, ~70× the figure above, and the volatile-scenario collapse described just above no longer reproduces. So **beating the baseline by one or two orders of magnitude is expected, not a red flag** — in particular it does not mean a change has broken the harness into a fast failure path. Check that the usual way (`run_flash1.sh audit <scenario>` → `Status: PASS` with a matching hash, `Verdict: VALID`); that is the real correctness gate. To attribute a throughput change, compare against a build of the parent commit, not against these figures. Treat `BASELINE_MPS` in `plot_bench.py` the same way: a floor marker, not a target.

### flash1 per-operation latency distribution

`bench/google/bench_flash1_replay.cpp` replays the flash1 order stream through one `OrderBook`, times **every message individually**, and reports a distribution (x = time of one operation, y = frequency) rather than a mean — `flash1_latency.png`, plus a `--hist-json` sidecar embedded in the record under the additive `flash1_latency` key. Opt-in via `--benchmark_filter='^BM_Flash1_'`; self-skips when `external/` is missing. Full detail in `docs/BENCHMARKING.md`; the four things that matter:

- It **deliberately breaks rule 2 of `BenchSupport.hpp`** (no clocks in the timed region) because here the timing *is* the measurement. The tax is measured, not assumed: `TscTimer.hpp` publishes the empty `rdtsc`-pair distribution as a "timer floor", and every benchmark has a `_NoTiming` twin with the instrumentation compiled out. **The ns/op this benchmark prints is instrumented** — the twin beside it is the real cost, and the gap is large (~+90% on `normal`) because the fences also forbid cross-operation overlap.
- Registered with **`->Iterations(1)`**, which short-circuits Google Benchmark's ramp so one repetition is exactly one stream replay. Without it, samples from the discarded warm-up trials blend into the published histogram.
- Replay semantics are **byte-identical to `bench/flash1/adapter.cpp`** (price flip, explicit-id `Order` ctor, IOC residual pull, modify as remove + re-add). Do not change one without the other.
- The gate is the **reject counts**, not a hash: zero rejected cancels means the stream never replayed (the generator plants ~2% duplicates), a flood means a fast failure path, and both `SkipWithError`. Correctness itself stays with `run_flash1.sh audit`.

On WSL2 the CPUID invariant-TSC bit is masked, so `TscTimer.hpp` accepts kernel corroboration (`tsc_known_freq`) and records which evidence it used; with none it publishes **ticks** and labels the chart accordingly. The TSC also steps ~39 counts at a time here, so the instrument's real resolution is ~10 ns — reported as `timer.resolution` and used to pick display bins.

Formatting: `.clang-format` is `BasedOnStyle: Google` (2-space indent). Run `clang-format -i` on changed files.

## Architecture

Two namespaces: `Exchange::Types` (value types in `include/types/`) and `Exchange::Engine` (logic in `include/engine/` + `src/engine/`). Engine headers pull the value types in with a `using namespace Exchange::Types;` *inside* `namespace Exchange::Engine` — never a using-directive at file scope, which would leak into every TU that includes the header.

### Strong-typedef value types (`include/types/`)
Every domain scalar is a distinct struct wrapping a raw underlying `T` — an integer (`OrderId`, `OrderPrice`, `OrderQuantity`, `OrderBookId`), a `time_point` (`OrderTime`), or a fixed-size char array (`Symbol`) — plus the `OrderSide` and `EngineError` enums. They are **not** interchangeable with their underlying `T` — construction is `explicit`, comparison is via defaulted `operator<=>`, and only `OrderQuantity` defines arithmetic. `OrderId`/`OrderBookId`/`Symbol` ship `std::hash` specializations so they can key `unordered_map`. There is no umbrella header; consumers include individual headers by path (`#include "types/OrderId.hpp"`). When adding a field, follow this pattern rather than passing bare `uint64_t`.

`Symbol` (`include/types/Symbol.hpp`) is the odd one out: a ticker held inline as a zero-padded `std::array<char, 8>` (`kMaxLength`) rather than a `std::string`, so it is trivially copyable, never allocates, and hashes as a single `std::bit_cast<uint64_t>`. The zero padding is load-bearing — it is what makes `operator==` and `std::hash` agree for tickers shorter than 8 chars, so **no constructor may name `value` in a mem-init-list** (that would bypass the `T value{}` NSDMI). The explicit `std::string_view` constructor treats `size() <= kMaxLength` as a caller-owned precondition; `Symbol::tryMake` is the validating path, returning `unexpected(SymbolTooLong)`. `view()` returns a `string_view` sized by scanning to the first `'\0'`, since a full 8-char ticker has no terminator. A block of `static_assert`s in the header pins these invariants.

IDs are assigned by a **`static inline instance_count` counter** on `Order` and `OrderBook` — every constructed `Order`/`OrderBook` auto-increments a global counter. IDs are process-wide monotonic, not per-book, and not user-supplied.

### Layering
- **`Order`** — a plain aggregate (price/time/quantity/side + id). Copyable/movable; `newOrderWithQuantity` returns a copy with an adjusted quantity.
- **`PriceLevel`** — `{ OrderPrice price; std::vector<Order> orders; }`. Orders within a level are time-ordered (FIFO) by insertion.
- **`OrderBook`** (`src/engine/OrderBook.cpp`) — owns two `std::vector<PriceLevel>` (`m_buy_levels`, `m_sell_levels`) kept **sorted worst-price first**: buys ascending, sells descending, so the best level is always `back()` on both sides. That orientation is deliberate — matching consumes from the best end, so a sweep erases a *suffix* and moves no other element, and a new best price is a `push_back`. Anything wanting price priority must walk these in reverse (`MarketDataPublisher::readWindow`, the flash1 adapter's best-bid/ask queries). Move-only, and **not default-constructible**: the sole constructor is `explicit OrderBook(Symbol)`, so every book names the one instrument it trades (`symbol()`). The id is still auto-assigned, never caller-supplied. Core operations:
  - `addOrder` matches the incoming (aggressing) order against the opposite side, then rests any remainder. Matching walks levels from the **back** (best price) towards the front and **breaks early** as soon as a level's best order fails the price predicate (`m_match_buy_aggressor` / `m_match_sell_aggressor`); the fully-consumed levels are then dropped as a single suffix erase.
  - `removeOrder` / `modifyOrder` — `modifyOrder` is currently implemented as remove-then-re-add (loses time priority by design).
  - A `std::unordered_map<OrderId, {OrderSide, OrderPrice}>` (`m_order_id_to_side_and_price`) lets removal locate an order's level without scanning both sides.
  - Side-generic traversal is done with **templated-on-`OrderSide`** helpers (`priceLevelIterator`, `tryInsertPriceLevel`) and a single `priceLevelIteratorImpl` using deducing-`this` to share const/non-const bodies.
- **`MatchingEngine`** (`src/engine/MatchingEngine.cpp`) — routes by `OrderBookId` over `unordered_map<OrderBookId, OrderBook>`, plus two secondary indexes: `OrderId -> OrderBookId` so order operations can be addressed by order id alone, and `Symbol -> OrderBookId` so a book can be addressed by ticker. Public methods return `std::expected<..., EngineError>` and compose via `.and_then(...)`; `getOrderBook` has const/non-const × by-order-id/by-book-id/by-symbol overloads, all delegating to a templated `getOrderBookImpl` (one overload per key type — they never collide, since the three key types are unrelated classes with `explicit` constructors).
  - `addOrderBook(OrderBook&&)` registers a book under **its own** symbol, returning `expected<OrderBookId, EngineError>`; a symbol already on file is rejected with `DuplicateSymbol` rather than replacing the live book. Both indexes use `try_emplace`, never `insert_or_assign`, so a registration can never silently orphan the order ids pointing at a replaced book.
  - **Symbols resolve, they do not route.** `resolve(Symbol) -> expected<OrderBookId, EngineError>` (or a `getOrderBook(Symbol)` lookup) is the API-boundary entry point; `addOrder` remains `OrderBookId`-addressed only, and deliberately has **no `Symbol` overload**, so symbol hashing can never creep onto the matching hot path. A symbol is a property of the *book*, not of an `Order` — `Order` carries no symbol.

### Error handling
Recoverable failures use `std::expected<T, EngineError>` (never exceptions). `EngineError` is `{ Success, OrderNotFound, OrderBookNotFound, SymbolNotFound, DuplicateSymbol, SymbolTooLong }`. Propagate with `std::unexpected(...)` and chain with `and_then`, matching the existing style.

## Networking (`include/net/`, `src/net/`)

### Build

`EXCHANGE_BUILD_NET` (ON by default) adds `src/net`. **Boost must be found in CONFIG mode** — CMake 4.4 removed `FindBoost.cmake`, so module mode hard-fails. The layer is header-only Boost (Asio + Beast + JSON) with `BOOST_{ASIO,BEAST}_SEPARATE_COMPILATION`; exactly one TU (`src/net/boost_impl.cpp`) carries the implementation, and adding anything to it costs a recompile of all of Asio.

| Target | Kind | Notes |
|---|---|---|
| `net_protocol` | INTERFACE | header-only core + wire |
| `net_boost` | INTERFACE | `Boost::headers`, Threads, separate-compilation defines |
| `net_gateway` | STATIC | `engine` + `net_protocol` + Threads. **Deliberately no Boost** — the matching-thread logic must stay exercisable with no sockets. The binary codec lives here for the same reason. |
| `net_io` | STATIC | sockets, sessions, listeners, the JSON codec, `boost_impl.cpp` |
| `exchange_server`, `exchange_cli`, `net_smoke` | EXE | |
| `net_loopback_bench`, `net_workload_bench` | EXE | Release only, `EXCLUDE_FROM_ALL`. See "Network benchmark pipeline". |

Nothing is SHARED, so the non-PIC `engine` never becomes a problem. `net_io` links `engine` **normally** and inherits its sanitizers — do NOT copy the flash1 adapter's compile-`OrderBook.cpp`-directly trick here, which would mismatch `std::vector<Fill>` layouts across the `_GLIBCXX_DEBUG` boundary.

Three build trees: `debug` (ASan/UBSan/LSan), `tsan` (ThreadSanitizer — the only thing that can validate the rings' memory ordering, and it cannot coexist with ASan), `release`.

### Threading

**Thread-per-`io_context`**, not one context with N threads. A socket is only ever touched by its own thread, so there are **no strands anywhere** and per-session state (write buffers, frame parser, in-flight counter) is plain non-atomic. Accepting happens on thread 0; sockets are handed off with `release()` + re-adopt on the target context — *moving the socket object alone silently misbehaves*, because it keeps its association with the original reactor.

`session_id = (io_thread_index << 24) | local_counter`. The high byte routes egress in O(1); the per-thread counter needs no atomic. 0 is never handed out — it is the "no session" sentinel.

Ingress: one `SpscRing<Command>` per I/O thread, round-robined by the matching thread with a per-ring batch cap. Chosen over an MPSC queue because per-session FIFO is the only ordering that matters and a pinned session gets it by construction; see the comment on `MatchingThread`.

### The one-minter rule

`Order::instance_count` and `OrderBook::instance_count` are non-atomic `static inline` counters. **`MatchingLoop` mints every order id itself and uses `Order`'s explicit-id constructor exclusively**, so that counter is never written in this process. `OrderBook` construction *does* bump its counter, which is why book creation must happen inside the `CreateBook` handler on the matching thread (a debug assert enforces the thread).

`OrderTime` is **priority, not a clock**: it comes from the loop's monotonic `m_seq`, which satisfies the book's non-decreasing invariant for free. Wall-clock for reports comes from `Command::recv_ts_ns`, stamped on the I/O thread. Never conflate them. Together these make a scripted `vector<Command>` produce a bit-identical `vector<Event>` every run.

### Wire

`Command` and `Event` are each **exactly 64 bytes**, trivially copyable, **no pointers ever**, `static_assert`ed. Because `Event` is fixed-size, a snapshot is not one event — it is `SnapshotBegin` + N×`LevelUpdate` + `SnapshotEnd` in one all-or-nothing `tryPushBatch`.

`Types::OrderSide` is a scoped enum with no fixed underlying type, so it is `int`-sized and cannot appear in those structs. `net/core/Side.hpp` is the 1-byte wire side, converted at the engine boundary — deliberately, rather than changing an engine header and moving `Order`'s benchmarked layout.

Binary protocol: 8-byte header, little-endian, fixed-size bodies ordered largest-first for zero padding, each `static_assert`ed. Decoded by **`memcpy`, never `reinterpret_cast`** (a frame boundary in a stream buffer has no alignment guarantee, and UBSan is on in Debug). Server→client types have the **high bit set**. `length` is validated against `[8, 1024]` before being trusted.

`MessageNames.hpp` is the single `MsgType <-> string_view` table used by **both** codecs, and JSON field names are byte-identical to the binary struct fields. `net_smoke` encodes the same content through both and compares the resulting `Command`/`Event` bit-for-bit — that test is the anti-drift mechanism, not a nicety.

**Prices are integers in both protocols.** The GUI divides by `10^price_scale`. No floats on the wire, ever.

### Backpressure — four conditions, four answers

| Condition | Policy |
|---|---|
| Ingress ring full | Suspend reads on **all** sessions of that thread (the ring is thread-wide) until it drains to a quarter free. TCP backpressure is the honest signal. |
| Per-session credit exhausted (64 in flight) | Immediate `Throttled` reject on the I/O thread. The counter is a plain `uint32_t` — same thread both ways, because the session is pinned. |
| Egress full, **private** stream | Disconnect. A dropped exec report silently desynchronizes the client's position and it cannot detect that. Resting orders survive. |
| Egress full, **market data** | Drop it. Every message carries `md_seq`, so the client detects the gap and resnapshots — that path exists for exactly this. |
| `SessionOpened`/`SessionClosed` | Must **never** be dropped. If the ring is full they go to `IoThread`'s critical queue, which outlives the session object. |

Never block on a full ring from an I/O thread.

### Market data

L2 depth deltas (default 10 levels/side, a property of the **book**, not the subscriber — `md_seq` is per book, so every subscriber must get the same message stream) plus a trade tape. `quantity == 0` means the level is gone.

Deltas are computed by diffing the **last published depth window** against the current one, not by looking up dirty prices. Same O(depth) cost, and it is the only formulation correct at the depth boundary: a deleted level makes the one at rank N visible, and a new best pushes one out of view, and neither is a "dirty price". See the long comment in `MarketDataPublisher.hpp`.

Sequencing is free: `SubscribeMd` travels the same ordered path as everything else, so the matching thread handles it *between* two commands with the book quiescent. The ring's total order gives snapshot/delta consistency with no gap window and no race.

### Ownership and sessions

All ownership state is on the matching thread — non-negotiable, because the check must precede `removeOrder` and there is no way to un-cancel. `OrderStore` holds `OrderId -> OrderMeta{trader, session, coid, book, price, side, original, leaves}`; `leaves = original - Σ fill.quantity` makes the gateway the authoritative order-state store the engine lacks. Insert only if `leaves > 0`, erase at 0 — exactly mirroring when the engine indexes and drops.

Ownership is by **trader**; routing is by **session**. Two clients on the same key see each other's fills. Cancel answers `UnknownOrder` from the store without touching the engine, which disambiguates `EngineError::OrderNotFound` and avoids a latent `end()` deref in `OrderBook::removeOrder`. `NotYourOrder` exists for the log only — `toWire` collapses it to `UnknownOrder` so the protocol is not an order-id enumeration oracle.

**Amend mints a new order id.** `OrderBook::modifyOrder` takes no new price or quantity and is unusable as an amend, so it is remove + re-add stamped with the *current* seq. Priority is lost even when shrinking quantity, because `tryInsertRestingOrder` push_backs regardless of time. This is protocol-visible and documented.

### Gotchas

- **`json::value` must not be brace-initialized.** `json::value v{json::parse(...)}` selects the `initializer_list` constructor and yields a one-element *array* wrapping the document. The repo brace-initializes everywhere; this type is the exception.
- **`std::println` is not concurrency-safe on this toolchain.** libstdc++'s `_File_sink` touches the `FILE` buffer outside the stdio lock. Use `net/io/Log.hpp` (`logLine`/`logError`) anywhere more than one thread can log.
- **`log` collides with `<cmath>`'s** outside `namespace Exchange::Net`, hence `logLine`.

### Verification

```bash
./build/debug/src/net/net_smoke   # rings, gateway scenarios, codecs, fuzz, loopback
./build/tsan/src/net/net_smoke    # the same, under ThreadSanitizer
scripts/net_e2e.sh                # binary protocol + market-data replay gate
scripts/ws_e2e.py                 # two browser clients trade; ladders agree
exchange_cli --load 8 --rate 50000 --book N --seconds 60   # multi-thread gate
```

`net_smoke` must pass under **both** the `debug` and `tsan` trees; that pair substitutes for a unit-test suite. `scripts/net_e2e.sh` is the market-data gate: `exchange_cli --tail --verify` rebuilds the book from a snapshot plus deltas and compares it against a fresh snapshot.

`net_loopback_bench` (Release only, `cmake --build --preset loopback-bench`) reports p50/p99/p99.9 for the gateway alone and for a full TCP round trip, across three pipeline depths and both egress implementations. Measured result: the lock-free egress and `asio::post` are indistinguishable at depth 1, reach parity around depth 8, and the ring pulls ~8-10% ahead on median and throughput by depth 32 where one eventfd wake carries ~40 events. At this project's real load it buys nothing measurable — which is what the design predicted and what the benchmark exists to check rather than assume. `--json PATH` writes the same numbers machine-readably; stdout is unchanged either way.

### Network benchmark pipeline

`net_workload_bench` (`bench/net/`, Release only, `cmake --build --preset net-bench`) replays the **flash1 order stream** through the networking layer, so the network numbers sit on the same axis as the engine numbers — same five volatility regimes, same messages, no second synthetic workload to keep honest. `scripts/net_bench_pipeline.sh` is the `bench_pipeline.sh` equivalent: run, record to `bench/results/net/run_<ts>_<sha>.json`, plot to `bench/results/net/plots/`.

```bash
scripts/net_bench_pipeline.sh                 # quick: 200k-order stream, all five scenarios
scripts/net_bench_pipeline.sh --full          # the canonical 1M-order stream, wider rate sweep
scripts/net_bench_pipeline.sh --scenario normal
scripts/net_bench_pipeline.sh --plot-only
```

The stream is **not** internal to the harness binary: `./generator` writes `orders_<scenario>_s<seed>_n<count>.bin` and the harness only shells out when it is missing, so `bench/net/Flash1Workload.hpp` reads the same files. It redeclares the 40-byte record rather than including a harness header, so the target builds with or without `external/` — only *running* needs it.

Two modes. `gateway` drives `Command`s straight through `MatchingLoop` with no sockets, and the gap between it and the flash1 harness's own M msgs/s is the price of the gateway's ownership, `leaves` and market data. `tcp` is open-loop: sends are paced at a target offered rate and **the latency clock starts at the scheduled send time, not the actual one**, so a driver that falls behind puts its own backlog in the number. That is the whole point — a closed-loop measurement cannot show a knee.

Three things are load-bearing and easy to get wrong:

- **Prices use the same sign-bit flip as `bench/flash1/adapter.cpp`.** That is what makes a tick land on the same `OrderPrice` on both paths. If the two ever diverge the scenarios stop being comparable.
- **Cancel and amend send `order_id = 0`**, resolving through `client_order_id` (`MatchingLoop::resolveTarget`), so the client replays generator-assigned ids without learning exchange-minted ones.
- **Acks are matched to commands in FIFO order, not by client order id.** The generator emits each order's cancel immediately after its new — the median gap is *one message* — so at any pipeline depth above 1 both are outstanding at once carrying the same coid, and a table keyed on it collides on roughly half of all messages. FIFO is exact: the session is pinned, commands are handled in ring order, egress preserves order. The coid on each ack is then a consistency check, and any mismatch is counted as `desync` and **fails the run**. The one command answering with two ack-family events is an IOC leaving a residual (`OrderAck` then `CancelAck`); it is recognised by coid and skipped.

What is deliberately *not* reproduced: `OrderTime` comes from `m_seq` rather than the workload's `seq` (same total order, so FIFO priority is preserved), and amend mints a new order id. There is no correctness hash here — correctness stays with `scripts/run_flash1.sh audit`. This gates instead on the reject counts: the generator plants ~2% duplicate cancels and stale modifies, so **zero `UnknownOrder` rejects means the stream did not really replay**, and a flood means the run found a fast failure path.

Reading the plots: `net_latency_vs_rate.png` is the one that matters — p50/p99/p99.9 against offered load, with saturation marked. The p50 and p99 curves are the signal; the p99.9 tail is noisy on a loaded desktop (three hot threads with `--spin-us 200`), so a non-monotonic p99.9 between two paced points is scheduler noise, not a knee.

**Phases 0-6 must not touch** `OrderBook.hpp`, `Order.hpp`, `PriceLevel.hpp` or `Symbol.hpp` — the flash1 harness compiles against them. Every engine change gets `scripts/run_flash1.sh audit` on all five scenarios before merge.

## Conventions

- Members prefixed `m_`; templated free functions and helpers stay in the header when they must be visible to callers.
- Prefer moving `Order&&` through the API; the engine takes ownership.
- The matching loop assumes `addOrder` is called with non-decreasing `time` (FIFO time priority relies on it) and positive quantity — noted in the source comments; preserve these invariants.
