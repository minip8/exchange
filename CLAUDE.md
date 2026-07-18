# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

A single-threaded, in-memory limit-order-book matching engine written in **C++26**. There is no networking, persistence, or user-facing protocol — it is a pure in-process library (`libengine.a`) plus a smoke-test `app` executable and Google Benchmark suites.

## Build & Run

The build uses **CMake + Ninja** with **g++** (see `build/CMakeCache.txt`). C++26 with `-fexperimental-library`-level features (`std::expected`, `std::print`, `std::function_ref`, explicit object parameters / "deducing `this`").

```bash
# Configure (Debug is the intended default — enables ASan/UBSan/LSan + _GLIBCXX_DEBUG)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug

# Build everything
cmake --build build

# Run the smoke-test executable
./build/app

# Build & run benchmarks (compiled with -O3 -march=native regardless of build type)
cmake --build build --target exchange_bench
./build/bench/exchange_bench

# Run a single benchmark by name (regex)
./build/bench/exchange_bench --benchmark_filter='BM_AddOrder.*'

# JSON output for regression gating (see bench/CMakeLists.txt for the intended CI flow)
./build/bench/exchange_bench --benchmark_format=json > bench_results.json
```

There is **no unit-test suite** — correctness is currently exercised only via `src/main.cpp` and the benchmarks. `Debug` builds carry the sanitizers, so run benches in a non-Debug config (`-DCMAKE_BUILD_TYPE=Release`) when measuring, but keep Debug for development so ASan/UBSan catch bugs.

Formatting: `.clang-format` is `BasedOnStyle: Google` (2-space indent). Run `clang-format -i` on changed files.

## Architecture

Two namespaces: `Exchange::Types` (value types in `include/types/`) and `Exchange::Engine` (logic in `include/engine/` + `src/engine/`).

### Strong-typedef value types (`include/types/`)
Every domain scalar is a distinct struct wrapping a raw integer/`time_point` (`OrderId`, `OrderPrice`, `OrderQuantity`, `OrderBookId`, `OrderTime`, plus the `OrderSide` enum and `EngineError`). They are **not** interchangeable with their underlying `T` — construction is `explicit`, comparison is via defaulted `operator<=>`, and only `OrderQuantity` defines arithmetic. `OrderId`/`OrderBookId` ship `std::hash` specializations so they can key `unordered_map`. When adding a field, follow this pattern rather than passing bare `uint64_t`.

IDs are assigned by a **`static inline instance_count` counter** on `Order` and `OrderBook` — every constructed `Order`/`OrderBook` auto-increments a global counter. IDs are process-wide monotonic, not per-book, and not user-supplied.

### Layering
- **`Order`** — a plain aggregate (price/time/quantity/side + id). Copyable/movable; `newOrderWithQuantity` returns a copy with an adjusted quantity.
- **`PriceLevel`** — `{ OrderPrice price; std::vector<Order> orders; }`. Orders within a level are time-ordered (FIFO) by insertion.
- **`OrderBook`** (`src/engine/OrderBook.cpp`) — owns two `std::vector<PriceLevel>` (`m_buy_levels`, `m_sell_levels`) kept **sorted by price-priority**: buys descending, sells ascending, so the best level is always `front()`. Move-only. Core operations:
  - `addOrder` matches the incoming (aggressing) order against the opposite side, then rests any remainder. Matching walks levels from the front and **breaks early** as soon as a level's best order fails the price predicate (`m_match_buy_aggressor` / `m_match_sell_aggressor`).
  - `removeOrder` / `modifyOrder` — `modifyOrder` is currently implemented as remove-then-re-add (loses time priority by design).
  - A `std::unordered_map<OrderId, {OrderSide, OrderPrice}>` (`m_order_id_to_side_and_price`) lets removal locate an order's level without scanning both sides.
  - Side-generic traversal is done with **templated-on-`OrderSide`** helpers (`priceLevelIterator`, `tryInsertPriceLevel`) and a single `priceLevelIteratorImpl` using deducing-`this` to share const/non-const bodies.
- **`MatchingEngine`** (`src/engine/MatchingEngine.cpp`) — routes by `OrderBookId` over `unordered_map<OrderBookId, OrderBook>`, plus an `OrderId -> OrderBookId` index so order operations can be addressed by order id alone. Public methods return `std::expected<..., EngineError>` and compose via `.and_then(...)`; `getOrderBook` has const/non-const/by-id/by-book-id overloads all delegating to a templated `getOrderBookImpl`.

### Error handling
Recoverable failures use `std::expected<T, EngineError>` (never exceptions). `EngineError` is `{ Success, OrderNotFound, OrderBookNotFound }`. Propagate with `std::unexpected(...)` and chain with `and_then`, matching the existing style.

## Conventions

- Members prefixed `m_`; templated free functions and helpers stay in the header when they must be visible to callers.
- Prefer moving `Order&&` through the API; the engine takes ownership.
- The matching loop assumes `addOrder` is called with non-decreasing `time` (FIFO time priority relies on it) and positive quantity — noted in the source comments; preserve these invariants.
