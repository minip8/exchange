#pragma once

/*
Shared fixture helpers for the Google Benchmark suite.

Two rules hold everywhere in this suite, and both exist because breaking them
silently produced numbers that looked plausible and were not:

  1. Nothing that is not the operation under test may run inside the timed
     region. That includes fixture *teardown* — a `for (auto _ : state)` body
     that owns an `OrderBook` destroys it after `ResumeTiming()`, so the
     measurement becomes the destructor. Use one of the two patterns below.

  2. No clock reads and no RNG draws inside the timed region. A
     `high_resolution_clock::now()` costs ~25 ns and an `mt19937_64` draw plus
     distribution costs ~10-15 ns, which is the same order as the operations
     being measured here. Times come from `seqTime`, randomness is
     pre-generated into a script before the loop.

The two fixture patterns:

  Pattern A — self-inverse pair. Where an operation has an exact inverse
  (add/remove, or modify, which is already remove-then-re-add), run the pair in
  the timed region so the book returns to its starting state. The fixture is
  built once outside the loop and the benchmark then needs no timer
  manipulation at all. What it reports is the round trip; say so in the name.

  Pattern B — `KeepRunningBatch` with the rebuild inside the paused region.
  Where the operation is destructive and has no inverse (matching). Holding the
  book in a `std::optional` and move-assigning inside the paused region puts the
  previous book's destructor there too — that is the whole point, and the thing
  the naive version gets wrong. One pause per batch instead of per operation
  also divides the ~640 ns `PauseTiming`/`ResumeTiming` cost down to noise.
*/

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "engine/Order.hpp"
#include "engine/OrderBook.hpp"
#include "types/OrderPrice.hpp"
#include "types/OrderQuantity.hpp"
#include "types/OrderSide.hpp"
#include "types/OrderTime.hpp"

/*
`Symbol` arrived in a925d17; before that `OrderBook` was default-constructible
and named no instrument. scripts/bench_backfill.sh compiles *this* file against
those older engines (see its legacy path), so book construction is the one thing
here that has to know which era it is in.

Nothing else does. Every operation these fixtures measure — addOrder,
removeOrder, modifyOrder, buys()/sells() — has the same signature at both ends
of the range, and this guard touches only fixture *construction*, never a timed
region. That is what keeps the measuring instrument identical across the trend,
which is the rule bench_backfill.sh exists to protect.
*/
#if __has_include("types/Symbol.hpp")
#include "types/Symbol.hpp"
#define EXCHANGE_BENCH_HAS_SYMBOL 1
#endif

namespace Exchange::Bench {

using namespace Exchange::Engine;
using namespace Exchange::Types;

using Clock = std::chrono::high_resolution_clock;

/*
The mid price every fixture is built around. Well clear of zero so a deep book
can price levels below it without underflowing OrderPrice's uint64_t.

`kMid` itself is deliberately left empty by the fixtures: bids start at
`kMid - 1` and offers at `kMid + 1`, so there is one free tick inside the
spread. Without it there is no price at which a *new best bid* can be inserted
without crossing, and BM_AddRemove_BestPrice_AtDepth — the benchmark that shows
the top-of-book insert is O(1) — cannot be written at all.
*/
inline constexpr OrderPrice::T kMid{10'000};
inline constexpr OrderPrice::T kBestBid{kMid - 1};
inline constexpr OrderPrice::T kBestAsk{kMid + 1};

/*
`OrderTime` is priority, not a clock — `MatchingLoop` stamps orders from its own
monotonic `m_seq` for exactly this reason. A counter satisfies `addOrder`'s
non-decreasing-time precondition for free and keeps a vDSO clock read off the
measured path.
*/
inline OrderTime seqTime(uint64_t seq) {
  return OrderTime{OrderTime::T{Clock::duration{static_cast<Clock::rep>(seq)}}};
}

// Deterministic seed so a script generated on one machine is the script
// generated on every other, and two runs of the same commit differ only by
// machine noise.
inline std::mt19937_64 benchRng() { return std::mt19937_64{42}; }

// An empty book, however this engine spells one. See the guard above.
inline OrderBook makeOrderBook(const char* name = "BENCH") {
#ifdef EXCHANGE_BENCH_HAS_SYMBOL
  return OrderBook{Symbol{name}};
#else
  (void)name;          // pre-a925d17: a book names no instrument
  return OrderBook{};  // no Symbol, default-constructible
#endif
}

/*
Builds a book with `depth` non-crossing price levels per side, each holding
`orders_per_level` orders of `quantity`, centred on `kMid`.

`seq` threads through so every order in a fixture — and every order a benchmark
subsequently adds to it — carries a strictly increasing time, which is the
precondition `addOrder` documents.
*/
inline OrderBook makeBook(uint64_t& seq, std::size_t depth,
                          std::size_t orders_per_level = 1,
                          OrderQuantity::T quantity = 100) {
  OrderBook book{makeOrderBook()};
  // Descending `i` means prices are added *worst first* on both sides, which is
  // the order the level vectors are already kept in — so every fixture insert
  // is a push_back. Building best-price-first instead makes each insert land at
  // begin() and the whole fixture quadratic in depth, which is enough to make
  // the paused rebuild dominate the suite's wall-clock time.
  for (std::size_t i = depth; i-- > 0;) {
    for (std::size_t n = 0; n < orders_per_level; ++n) {
      book.addOrder(Order{OrderPrice{kBestBid - i}, seqTime(++seq),
                          OrderQuantity{quantity}, OrderSide::Buy});
      book.addOrder(Order{OrderPrice{kBestAsk + i}, seqTime(++seq),
                          OrderQuantity{quantity}, OrderSide::Sell});
    }
  }
  return book;
}

/*
Sells only. A buy aggressor never touches the bid side, so building it doubles
the paused rebuild cost of every matching benchmark for nothing — and those
rebuilds are the dominant wall-clock cost of the suite once the timing is
correct.
*/
inline OrderBook makeSellBook(uint64_t& seq, std::size_t depth,
                              std::size_t orders_per_level = 1,
                              OrderQuantity::T quantity = 100) {
  OrderBook book{makeOrderBook()};
  // Worst (highest) offer first — see makeBook.
  for (std::size_t i = depth; i-- > 0;) {
    for (std::size_t n = 0; n < orders_per_level; ++n) {
      book.addOrder(Order{OrderPrice{kBestAsk + i}, seqTime(++seq),
                          OrderQuantity{quantity}, OrderSide::Sell});
    }
  }
  return book;
}

/*
Ids of every order resting on the bid side, best level first.

"Best first" holds only from 586ecc6 onwards, which reversed the level ordering.
Backfilled against an older engine this yields worst-first instead. Harmless:
both callers (BM_ModifyOrder, BM_MixedWorkload_SteadyState) use the result as an
unordered pool or take a midpoint from it, so neither depends on which end is
which. The two _AtDepth benchmarks, which *do* care, sidestep this entirely by
addressing their level by price rather than by position.
*/
inline std::vector<OrderId> restingBuyIds(const OrderBook& book) {
  std::vector<OrderId> ids;
  const auto levels = book.buys();
  // buys() is worst-price first, so walk it in reverse for price priority.
  for (auto it = levels.rbegin(); it != levels.rend(); ++it) {
    for (const Order& order : it->orders) ids.push_back(order.id);
  }
  return ids;
}

}  // namespace Exchange::Bench
