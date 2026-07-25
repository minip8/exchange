#include <benchmark/benchmark.h>

#include <random>
#include <vector>

#include "engine/Order.hpp"
#include "engine/OrderBook.hpp"
#include "types/OrderPrice.hpp"
#include "types/OrderQuantity.hpp"
#include "types/OrderSide.hpp"
#include "types/OrderTime.hpp"
#include "types/Symbol.hpp"

using namespace Exchange::Engine;
using namespace Exchange::Types;

namespace {

OrderTime now() { return OrderTime{std::chrono::high_resolution_clock::now()}; }

// Builds a book with `depth` non-crossing price levels on each side,
// centered around 10'000, so a benchmark can start from a realistic
// (not empty) book state.
OrderBook makeBookWithDepth(std::size_t depth,
                            Symbol symbol = Symbol{"BENCH"}) {
  OrderBook book{symbol};
  for (std::size_t i = 0; i < depth; ++i) {
    book.addOrder(Order{OrderPrice{10'000 - i}, now(), OrderQuantity{100},
                        OrderSide::Buy});
    book.addOrder(Order{OrderPrice{10'001 + i}, now(), OrderQuantity{100},
                        OrderSide::Sell});
  }
  return book;
}

// Deterministic seed so results are comparable across runs/CI.
std::mt19937_64& rng() {
  static std::mt19937_64 gen{42};
  return gen;
}

}  // namespace

// ---------------------------------------------------------------------
// Insert: order rests (no match), book empty.
// Measures pure "add a resting order to an empty book" cost.
// ---------------------------------------------------------------------
static void BM_AddOrder_NoMatch_EmptyBook(benchmark::State& state) {
  for (auto _ : state) {
    state.PauseTiming();
    OrderBook book{Symbol{"BENCH"}};
    state.ResumeTiming();

    auto fills = book.addOrder(
        Order{OrderPrice{10'000}, now(), OrderQuantity{100}, OrderSide::Buy});
    benchmark::DoNotOptimize(fills);
  }
}
BENCHMARK(BM_AddOrder_NoMatch_EmptyBook);

// ---------------------------------------------------------------------
// Insert into a book with existing depth, still non-crossing.
// `state.range(0)` is the number of pre-populated price levels per side.
// Because your price-level lookup is a linear std::ranges::find_if over
// a std::vector<PriceLevel>, this should show ~O(depth) growth — that's
// the key thing this benchmark is meant to surface.
// ---------------------------------------------------------------------
static void BM_AddOrder_NoMatch_AtDepth(benchmark::State& state) {
  const auto depth = static_cast<std::size_t>(state.range(0));

  for (auto _ : state) {
    state.PauseTiming();
    OrderBook book = makeBookWithDepth(depth);
    state.ResumeTiming();

    // Price sits one tick worse than best bid -> new level, worst case
    // for a linear scan (has to walk past every existing level).
    auto fills = book.addOrder(Order{OrderPrice{10'000 - depth}, now(),
                                     OrderQuantity{100}, OrderSide::Buy});
    benchmark::DoNotOptimize(fills);
  }
  state.SetComplexityN(static_cast<int64_t>(depth));
}
BENCHMARK(BM_AddOrder_NoMatch_AtDepth)
    ->RangeMultiplier(4)
    ->Range(1, 4096)
    ->Complexity();

// ---------------------------------------------------------------------
// Insert that crosses the book and matches immediately (full fill
// against top of book). Isolates matching/fill-generation cost from
// insertion cost.
// ---------------------------------------------------------------------
static void BM_AddOrder_Match_TopOfBook(benchmark::State& state) {
  for (auto _ : state) {
    state.PauseTiming();
    OrderBook book = makeBookWithDepth(10);
    state.ResumeTiming();

    // Aggressive buy that crosses the best offer.
    auto fills = book.addOrder(
        Order{OrderPrice{10'050}, now(), OrderQuantity{100}, OrderSide::Buy});
    benchmark::DoNotOptimize(fills);
  }
}
BENCHMARK(BM_AddOrder_Match_TopOfBook);

// ---------------------------------------------------------------------
// Insert that sweeps through multiple price levels before resting
// (or fully filling). This stresses the matching loop, not just the
// single-level match path.
// ---------------------------------------------------------------------
static void BM_AddOrder_Match_SweepLevels(benchmark::State& state) {
  const auto levels_to_sweep = static_cast<std::size_t>(state.range(0));

  for (auto _ : state) {
    state.PauseTiming();
    OrderBook book = makeBookWithDepth(levels_to_sweep + 5);
    state.ResumeTiming();

    // Large aggressive buy priced to walk through `levels_to_sweep`
    // sell levels.
    auto fills = book.addOrder(
        Order{OrderPrice{10'001 + levels_to_sweep}, now(),
              OrderQuantity{100 * levels_to_sweep}, OrderSide::Buy});
    benchmark::DoNotOptimize(fills);
  }
  state.SetComplexityN(static_cast<int64_t>(levels_to_sweep));
}
BENCHMARK(BM_AddOrder_Match_SweepLevels)
    ->RangeMultiplier(2)
    ->Range(1, 256)
    ->Complexity();

// ---------------------------------------------------------------------
// Cancel: remove an existing resting order by id.
// Book depth is varied since removeOrder has to find the order's price
// level too.
// ---------------------------------------------------------------------
static void BM_RemoveOrder_AtDepth(benchmark::State& state) {
  const auto depth = static_cast<std::size_t>(state.range(0));

  for (auto _ : state) {
    state.PauseTiming();
    OrderBook book = makeBookWithDepth(depth);
    // Remember an id we know is resting (first buy order added has id 0
    // only if this is the first OrderBook ever constructed in the
    // process -- safer to fetch it from the book's own state instead of
    // assuming ids).
    auto buys = book.buys();
    OrderId target_id =
        buys.empty() ? OrderId{0} : buys.front().orders.front().id;
    state.ResumeTiming();

    auto removed = book.removeOrder(target_id);
    benchmark::DoNotOptimize(removed);
  }
  state.SetComplexityN(static_cast<int64_t>(depth));
}
BENCHMARK(BM_RemoveOrder_AtDepth)
    ->RangeMultiplier(4)
    ->Range(1, 4096)
    ->Complexity();

// ---------------------------------------------------------------------
// Sustained mixed workload: repeated add/cancel churn at a fixed depth,
// approximating realistic order flow (mostly adds and cancels near the
// top of book, occasional matches). This is the throughput number worth
// tracking release-over-release.
// ---------------------------------------------------------------------
static void BM_MixedWorkload_SteadyState(benchmark::State& state) {
  std::uniform_int_distribution<int> side_dist(0, 1);
  std::uniform_int_distribution<uint64_t> price_jitter(0, 20);

  for (auto _ : state) {
    state.PauseTiming();
    OrderBook book = makeBookWithDepth(50);
    state.ResumeTiming();

    for (int i = 0; i < 1000; ++i) {
      const bool is_buy = side_dist(rng()) == 0;
      const auto jitter = price_jitter(rng());
      auto fills = book.addOrder(
          Order{OrderPrice{is_buy ? 10'000 - jitter : 10'001 + jitter}, now(),
                OrderQuantity{10}, is_buy ? OrderSide::Buy : OrderSide::Sell});
      benchmark::DoNotOptimize(fills);
    }
  }
  state.SetItemsProcessed(state.iterations() * 1000);
}
BENCHMARK(BM_MixedWorkload_SteadyState)->Unit(benchmark::kMicrosecond);