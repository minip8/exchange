#include <benchmark/benchmark.h>

#include <random>

#include "engine/MatchingEngine.hpp"
#include "engine/Order.hpp"
#include "engine/OrderBook.hpp"
#include "types/OrderBookId.hpp"
#include "types/OrderPrice.hpp"
#include "types/OrderQuantity.hpp"
#include "types/OrderSide.hpp"
#include "types/OrderTime.hpp"

using namespace Exchange::Engine;
using namespace Exchange::Types;

// Same API assumptions as bench_orderbook.cpp for OrderQuantity/OrderSide.

namespace {
// OrderTime wraps std::chrono::time_point<high_resolution_clock>, not a raw
// integer. Build increasing, deterministic timestamps off an arbitrary but
// fixed epoch offset so ordering is stable across benchmark runs.
OrderTime makeTime(uint64_t i) {
  return OrderTime{OrderTime::T{std::chrono::nanoseconds{i}}};
}
}  // namespace

// -----------------------------------------------------------------------
// End-to-end add, through MatchingEngine::addOrder(OrderBookId, Order).
// This adds the unordered_map hash+lookup for OrderBookId resolution on
// top of whatever BM_OrderBook_AddOrder_NoMatch already measures — diff
// the two numbers to see the routing overhead in isolation.
// -----------------------------------------------------------------------
static void BM_MatchingEngine_AddOrder_NoMatch(benchmark::State& state) {
  constexpr uint64_t kMid = 10'000;

  MatchingEngine engine;
  OrderBook book;
  const OrderBookId book_id = book.id();
  engine.addOrderBook(std::move(book));

  uint64_t i = 0;
  for (auto _ : state) {
    Order o{OrderPrice{kMid - 1}, makeTime(i++), OrderQuantity{100},
             OrderSide::Buy};
    auto result = engine.addOrder(book_id, std::move(o));
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_MatchingEngine_AddOrder_NoMatch);

// -----------------------------------------------------------------------
// End-to-end cancel, through MatchingEngine::removeOrder(OrderId). This
// exercises the OrderId -> OrderBookId map lookup followed by the
// OrderBook-level erase — the realistic cancel path a client actually hits.
// -----------------------------------------------------------------------
static void BM_MatchingEngine_RemoveOrder(benchmark::State& state) {
  const auto depth = static_cast<std::size_t>(state.range(0));
  constexpr uint64_t kMid = 10'000;

  MatchingEngine engine;
  OrderBook book;
  const OrderBookId book_id = book.id();
  engine.addOrderBook(std::move(book));

  // Static resting depth — held constant, never removed.
  for (std::size_t i = 0; i < depth; ++i) {
    Order o{OrderPrice{kMid - 1}, makeTime(i), OrderQuantity{100},
             OrderSide::Buy};
    engine.addOrder(book_id, std::move(o));
  }

  // One "churn" order added (untimed) and removed (timed) per iteration —
  // see comment in BM_OrderBook_RemoveOrder for why this replaces a fixed
  // pre-seeded pool of ids.
  uint64_t churn_time = depth;
  for (auto _ : state) {
    state.PauseTiming();
    Order churn{OrderPrice{kMid - 1}, makeTime(churn_time++),
                OrderQuantity{100}, OrderSide::Buy};
    const OrderId id = churn.id;
    engine.addOrder(book_id, std::move(churn));
    state.ResumeTiming();

    auto removed = engine.removeOrder(id);
    benchmark::DoNotOptimize(removed);
  }
}
BENCHMARK(BM_MatchingEngine_RemoveOrder)->Arg(1000)->Arg(10000);

// -----------------------------------------------------------------------
// getOrderBook(OrderId) lookup cost in isolation — pure map-lookup, no
// insert/erase side effects. Useful for seeing the floor cost of routing.
// -----------------------------------------------------------------------
static void BM_MatchingEngine_GetOrderBook_ByOrderId(benchmark::State& state) {
  constexpr uint64_t kMid = 10'000;

  MatchingEngine engine;
  OrderBook book;
  const OrderBookId book_id = book.id();
  engine.addOrderBook(std::move(book));

  Order o{OrderPrice{kMid - 1}, makeTime(0), OrderQuantity{100},
           OrderSide::Buy};
  const OrderId id = o.id;
  engine.addOrder(book_id, std::move(o));

  for (auto _ : state) {
    auto result = engine.getOrderBook(id);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_MatchingEngine_GetOrderBook_ByOrderId);