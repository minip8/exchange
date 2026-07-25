# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

A single-threaded, in-memory limit-order-book matching engine written in **C++26**. There is no networking, persistence, or user-facing protocol — it is a pure in-process library (`libengine.a`) plus a smoke-test `app` executable and Google Benchmark suites.

## Build & Run

The build uses **CMake + Ninja** with **g++**, driven entirely through `CMakePresets.json`. C++26 with `-fexperimental-library`-level features (`std::expected`, `std::print`, `std::function_ref`, explicit object parameters / "deducing `this`").

Two **single-config** trees, one per config, each pinned by a preset: `build/debug/` (the development default — ASan/UBSan/LSan + `_GLIBCXX_DEBUG`) and `build/release/` (optimized, sanitizer-free, the only config benchmarks may run in). Because the trees are single-config, artifacts land directly in the tree with **no per-config subdirectory**, and `--config` is never used — passing it to a single-config tree is silently ignored, which is exactly the failure this layout exists to prevent. Always address a tree by its preset, never by an ad-hoc `-B` path.

```bash
# Configure (once per tree; re-running is idempotent and cheap)
cmake --preset debug
cmake --preset release

# Build
cmake --build --preset debug      # everything, Debug
cmake --build --preset release    # everything, Release
cmake --build --preset bench      # just exchange_bench (Release)
cmake --build --preset flash1     # just the flash1 adapter (Release)

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

There is **no unit-test suite** — correctness is currently exercised only via `src/main.cpp`, the benchmarks, and the flash1 conformance harness (the real gate; see below). `src/main.cpp` is a hand-rolled smoke test: it checks with a local `check()` helper rather than `assert`, because Release defines `NDEBUG` and would compile the assertions away. It prints each failure and exits non-zero, so it is usable as a CI step.

`bench/CMakeLists.txt` is a pure dispatcher over two peer suites, each owning its own `CMakeLists.txt`: `bench/google/` (the Google Benchmark microbenchmarks — target `exchange_bench`, which also pins the googlebenchmark FetchContent dependency) and `bench/flash1/` (the external conformance harness adapter). Benchmark results and plots land in the gitignored `bench/results/`.

**Always benchmark the Release config.** `exchange_bench` sets `-O3 -march=native` on itself in every config, but it links `engine`, which propagates `debug_options` as a PUBLIC dependency — so a bench binary built in `build/debug/` is still built with ASan/UBSan/LSan and `_GLIBCXX_DEBUG`. Optimized *and* sanitized numbers are meaningless. Keep Debug for development so the sanitizers catch bugs.

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

`scripts/bench_pipeline.sh` runs the Google Benchmark suite (Release) plus one flash1 `perf` rep per scenario, writes a timestamped + git-SHA-tagged record JSON to gitignored `bench/results/`, and renders PNGs (latest snapshot + trend over history) to `bench/results/plots/`. Flags: `--full` (5 reps/scenario, median recorded — challenge-style scoring), `--skip-flash1`, `--plot-only`. Plotting needs `uv`; Python deps are declared inline in `scripts/plot_bench.py` (PEP 723). The flash1 baseline used for reference lines is hardcoded in `plot_bench.py` (`BASELINE_MPS`) — keep it in sync with the numbers below.

**The score is the worst-case scenario, not the average.** Baseline (WSL2, July 2026): normal 1.91 M/s, static 1.12, swing-25 0.16, swing-40 0.092, flash-crash 0.059 — so 0.059 M/s. The collapse on volatile scenarios is attributed to the vector book's linear level scans.

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
- **`OrderBook`** (`src/engine/OrderBook.cpp`) — owns two `std::vector<PriceLevel>` (`m_buy_levels`, `m_sell_levels`) kept **sorted by price-priority**: buys descending, sells ascending, so the best level is always `front()`. Move-only, and **not default-constructible**: the sole constructor is `explicit OrderBook(Symbol)`, so every book names the one instrument it trades (`symbol()`). The id is still auto-assigned, never caller-supplied. Core operations:
  - `addOrder` matches the incoming (aggressing) order against the opposite side, then rests any remainder. Matching walks levels from the front and **breaks early** as soon as a level's best order fails the price predicate (`m_match_buy_aggressor` / `m_match_sell_aggressor`).
  - `removeOrder` / `modifyOrder` — `modifyOrder` is currently implemented as remove-then-re-add (loses time priority by design).
  - A `std::unordered_map<OrderId, {OrderSide, OrderPrice}>` (`m_order_id_to_side_and_price`) lets removal locate an order's level without scanning both sides.
  - Side-generic traversal is done with **templated-on-`OrderSide`** helpers (`priceLevelIterator`, `tryInsertPriceLevel`) and a single `priceLevelIteratorImpl` using deducing-`this` to share const/non-const bodies.
- **`MatchingEngine`** (`src/engine/MatchingEngine.cpp`) — routes by `OrderBookId` over `unordered_map<OrderBookId, OrderBook>`, plus two secondary indexes: `OrderId -> OrderBookId` so order operations can be addressed by order id alone, and `Symbol -> OrderBookId` so a book can be addressed by ticker. Public methods return `std::expected<..., EngineError>` and compose via `.and_then(...)`; `getOrderBook` has const/non-const × by-order-id/by-book-id/by-symbol overloads, all delegating to a templated `getOrderBookImpl` (one overload per key type — they never collide, since the three key types are unrelated classes with `explicit` constructors).
  - `addOrderBook(OrderBook&&)` registers a book under **its own** symbol, returning `expected<OrderBookId, EngineError>`; a symbol already on file is rejected with `DuplicateSymbol` rather than replacing the live book. Both indexes use `try_emplace`, never `insert_or_assign`, so a registration can never silently orphan the order ids pointing at a replaced book.
  - **Symbols resolve, they do not route.** `resolve(Symbol) -> expected<OrderBookId, EngineError>` (or a `getOrderBook(Symbol)` lookup) is the API-boundary entry point; `addOrder` remains `OrderBookId`-addressed only, and deliberately has **no `Symbol` overload**, so symbol hashing can never creep onto the matching hot path. A symbol is a property of the *book*, not of an `Order` — `Order` carries no symbol.

### Error handling
Recoverable failures use `std::expected<T, EngineError>` (never exceptions). `EngineError` is `{ Success, OrderNotFound, OrderBookNotFound, SymbolNotFound, DuplicateSymbol, SymbolTooLong }`. Propagate with `std::unexpected(...)` and chain with `and_then`, matching the existing style.

## Conventions

- Members prefixed `m_`; templated free functions and helpers stay in the header when they must be visible to callers.
- Prefer moving `Order&&` through the API; the engine takes ownership.
- The matching loop assumes `addOrder` is called with non-decreasing `time` (FIFO time priority relies on it) and positive quantity — noted in the source comments; preserve these invariants.
