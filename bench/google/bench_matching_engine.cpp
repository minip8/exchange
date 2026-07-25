#include <benchmark/benchmark.h>

#include <random>
#include <string>
#include <vector>

#include "engine/MatchingEngine.hpp"
#include "engine/Order.hpp"
#include "types/OrderBookId.hpp"
#include "types/OrderId.hpp"
#include "types/OrderPrice.hpp"
#include "types/OrderQuantity.hpp"
#include "types/OrderSide.hpp"
#include "types/OrderTime.hpp"
#include "types/Symbol.hpp"

using namespace Exchange::Engine;
using namespace Exchange::Types;

namespace {

OrderTime now() { return OrderTime{std::chrono::high_resolution_clock::now()}; }

// Builds an engine with `n_books` books, each pre-populated with a
// resting order so lookups/inserts land in a non-trivial state, and
// returns the ids of orders that are known to be resting (for
// removeOrder / getOrderBook-by-OrderId benchmarks).
struct SeededEngine {
  MatchingEngine engine;
  std::vector<OrderBookId> book_ids;
  std::vector<Symbol> symbols;
  std::vector<OrderId> resting_order_ids;
};

// Every book needs a distinct symbol: addOrderBook rejects a duplicate, so a
// shared ticker would register only book 0 and leave every later lookup taking
// the not-found path — the sweep would silently measure a failure path.
Symbol symbolFor(std::size_t i) { return Symbol{std::to_string(i)}; }

SeededEngine makeEngine(std::size_t n_books) {
  SeededEngine seeded;
  for (std::size_t i = 0; i < n_books; ++i) {
    const auto symbol = symbolFor(i);
    // .value() so a registration failure is loud rather than silently
    // benchmarking an empty engine.
    const auto book_id = seeded.engine.addOrderBook(OrderBook{symbol}).value();
    seeded.book_ids.push_back(book_id);
    seeded.symbols.push_back(symbol);

    Order resting{OrderPrice{10'000}, now(), OrderQuantity{100},
                  OrderSide::Buy};
    seeded.resting_order_ids.push_back(resting.id);
    auto _ = seeded.engine.addOrder(book_id, std::move(resting));
  }
  return seeded;
}

std::mt19937_64& rng() {
  static std::mt19937_64 gen{42};
  return gen;
}

}  // namespace

// ---------------------------------------------------------------------
// getOrderBook(OrderBookId) — direct hash lookup, no indirection.
// This is the baseline routing cost floor.
// ---------------------------------------------------------------------
static void BM_GetOrderBook_ById(benchmark::State& state) {
  const auto n_books = static_cast<std::size_t>(state.range(0));
  auto seeded = makeEngine(n_books);
  std::uniform_int_distribution<std::size_t> pick(0, n_books - 1);

  for (auto _ : state) {
    const auto& book_id = seeded.book_ids[pick(rng())];
    auto result = seeded.engine.getOrderBook(book_id);
    benchmark::DoNotOptimize(result);
  }
  state.SetComplexityN(static_cast<int64_t>(n_books));
}
BENCHMARK(BM_GetOrderBook_ById)
    ->RangeMultiplier(8)
    ->Range(1, 8192)
    ->Complexity();

// ---------------------------------------------------------------------
// getOrderBook(OrderId) — indirect lookup: OrderId -> OrderBookId,
// then OrderBookId -> OrderBook. Two hash lookups instead of one;
// isolates the cost of the indirection layer itself.
// ---------------------------------------------------------------------
static void BM_GetOrderBook_ByOrderId(benchmark::State& state) {
  const auto n_books = static_cast<std::size_t>(state.range(0));
  auto seeded = makeEngine(n_books);
  std::uniform_int_distribution<std::size_t> pick(0, n_books - 1);

  for (auto _ : state) {
    const auto& order_id = seeded.resting_order_ids[pick(rng())];
    auto result = seeded.engine.getOrderBook(order_id);
    benchmark::DoNotOptimize(result);
  }
  state.SetComplexityN(static_cast<int64_t>(n_books));
}
BENCHMARK(BM_GetOrderBook_ByOrderId)
    ->RangeMultiplier(8)
    ->Range(1, 8192)
    ->Complexity();

// ---------------------------------------------------------------------
// getOrderBook(Symbol) — the ticker-addressed entry point: Symbol ->
// OrderBookId, then OrderBookId -> OrderBook. Same two-lookup shape as
// the OrderId path, so comparing the two isolates the cost of hashing an
// 8-byte inline symbol against hashing a uint64 id.
// ---------------------------------------------------------------------
static void BM_GetOrderBook_BySymbol(benchmark::State& state) {
  const auto n_books = static_cast<std::size_t>(state.range(0));
  auto seeded = makeEngine(n_books);
  std::uniform_int_distribution<std::size_t> pick(0, n_books - 1);

  for (auto _ : state) {
    const auto& symbol = seeded.symbols[pick(rng())];
    auto result = seeded.engine.getOrderBook(symbol);
    benchmark::DoNotOptimize(result);
  }
  state.SetComplexityN(static_cast<int64_t>(n_books));
}
BENCHMARK(BM_GetOrderBook_BySymbol)
    ->RangeMultiplier(8)
    ->Range(1, 8192)
    ->Complexity();

// ---------------------------------------------------------------------
// addOrder routed through the engine (as opposed to calling addOrder
// directly on an OrderBook, as in bench_orderbook.cpp). Measures the
// added cost of the routing layer on top of the raw insert.
// ---------------------------------------------------------------------
static void BM_Engine_AddOrder_Routed(benchmark::State& state) {
  const auto n_books = static_cast<std::size_t>(state.range(0));

  for (auto _ : state) {
    state.PauseTiming();
    auto seeded = makeEngine(n_books);
    std::uniform_int_distribution<std::size_t> pick(0, n_books - 1);
    const auto& target_book_id = seeded.book_ids[pick(rng())];
    state.ResumeTiming();

    auto fills = seeded.engine.addOrder(
        target_book_id,
        Order{OrderPrice{9'999}, now(), OrderQuantity{50}, OrderSide::Buy});
    benchmark::DoNotOptimize(fills);
  }
  state.SetComplexityN(static_cast<int64_t>(n_books));
}
BENCHMARK(BM_Engine_AddOrder_Routed)
    ->RangeMultiplier(8)
    ->Range(1, 8192)
    ->Complexity();

// ---------------------------------------------------------------------
// removeOrder — full path: OrderId -> OrderBookId lookup -> OrderBook
// -> per-level erase.
// ---------------------------------------------------------------------
static void BM_Engine_RemoveOrder(benchmark::State& state) {
  const auto n_books = static_cast<std::size_t>(state.range(0));

  for (auto _ : state) {
    state.PauseTiming();
    auto seeded = makeEngine(n_books);
    std::uniform_int_distribution<std::size_t> pick(0, n_books - 1);
    const auto target_id = seeded.resting_order_ids[pick(rng())];
    state.ResumeTiming();

    auto removed = seeded.engine.removeOrder(target_id);
    benchmark::DoNotOptimize(removed);
  }
  state.SetComplexityN(static_cast<int64_t>(n_books));
}
BENCHMARK(BM_Engine_RemoveOrder)
    ->RangeMultiplier(8)
    ->Range(1, 8192)
    ->Complexity();

// ---------------------------------------------------------------------
// addOrderBook — cost of registering a new book (hash insert). Worth
// tracking separately since this happens at instrument-listing time,
// not on the hot path, so a slow-but-not-hot version is fine — but you
// still want to know if it's accidentally O(n) in existing book count.
// ---------------------------------------------------------------------
static void BM_Engine_AddOrderBook(benchmark::State& state) {
  const auto n_existing = static_cast<std::size_t>(state.range(0));

  for (auto _ : state) {
    state.PauseTiming();
    auto seeded = makeEngine(n_existing);
    // A symbol outside [0, n_existing) so the registration actually succeeds.
    OrderBook new_book{symbolFor(n_existing)};
    state.ResumeTiming();

    (void)seeded.engine.addOrderBook(std::move(new_book));
  }
  state.SetComplexityN(static_cast<int64_t>(n_existing));
}
BENCHMARK(BM_Engine_AddOrderBook)
    ->RangeMultiplier(8)
    ->Range(1, 8192)
    ->Complexity();

// ---------------------------------------------------------------------
// Multi-book steady-state throughput: round-robins order flow across
// many books concurrently active in the engine, approximating a
// venue running many instruments at once rather than a single book in
// isolation.
// ---------------------------------------------------------------------
static void BM_Engine_MultiBook_SteadyState(benchmark::State& state) {
  const auto n_books = static_cast<std::size_t>(state.range(0));
  std::uniform_int_distribution<std::size_t> book_pick(0, n_books - 1);
  std::uniform_int_distribution<int> side_dist(0, 1);
  std::uniform_int_distribution<uint64_t> price_jitter(0, 20);

  for (auto _ : state) {
    state.PauseTiming();
    auto seeded = makeEngine(n_books);
    state.ResumeTiming();

    for (int i = 0; i < 1000; ++i) {
      const auto& book_id = seeded.book_ids[book_pick(rng())];
      const bool is_buy = side_dist(rng()) == 0;
      const auto jitter = price_jitter(rng());
      auto fills = seeded.engine.addOrder(
          book_id,
          Order{OrderPrice{is_buy ? 10'000 - jitter : 10'001 + jitter}, now(),
                OrderQuantity{10}, is_buy ? OrderSide::Buy : OrderSide::Sell});
      benchmark::DoNotOptimize(fills);
    }
  }
  state.SetItemsProcessed(state.iterations() * 1000);
}
BENCHMARK(BM_Engine_MultiBook_SteadyState)
    ->Arg(1)
    ->Arg(100)
    ->Arg(5000)
    ->Unit(benchmark::kMicrosecond);