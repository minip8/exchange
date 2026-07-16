#include <benchmark/benchmark.h>

#include <random>
#include <vector>

#include "engine/Order.hpp"
#include "engine/OrderBook.hpp"
#include "types/OrderPrice.hpp"
#include "types/OrderQuantity.hpp"
#include "types/OrderSide.hpp"
#include "types/OrderTime.hpp"

using namespace Exchange::Engine;
using namespace Exchange::Types;

// ASSUMPTION: OrderQuantity/OrderTime follow the same explicit-T-value
// pattern as OrderPrice. OrderSide is assumed to be `enum class Side {
// Buy, Sell }` (or similar). Adjust the three lines below if the real
// API differs — everything else is agnostic to that.
namespace {

// OrderTime wraps std::chrono::time_point<high_resolution_clock>, not a raw
// integer. Build increasing, deterministic timestamps off an arbitrary but
// fixed epoch offset so ordering is stable across benchmark runs.
OrderTime makeTime(uint64_t i) {
  return OrderTime{OrderTime::T{std::chrono::nanoseconds{i}}};
}

std::vector<Order> makeRestingBuys(std::size_t n, uint64_t mid_price) {
  std::vector<Order> orders;
  orders.reserve(n);
  std::mt19937_64 rng(42);  // fixed seed: reproducible across runs
  std::normal_distribution<double> price_jitter(0.0, 25.0);

  for (std::size_t i = 0; i < n; ++i) {
    auto jitter = static_cast<int64_t>(price_jitter(rng));
    uint64_t price = mid_price - 1 - static_cast<uint64_t>(std::max<int64_t>(0, jitter));
    orders.emplace_back(OrderPrice{price}, makeTime(i), OrderQuantity{100},
                         OrderSide::Buy);
  }
  return orders;
}

std::vector<Order> makeNonCrossingSells(std::size_t n, uint64_t mid_price) {
  std::vector<Order> orders;
  orders.reserve(n);
  std::mt19937_64 rng(43);
  std::normal_distribution<double> price_jitter(0.0, 25.0);

  for (std::size_t i = 0; i < n; ++i) {
    auto jitter = static_cast<int64_t>(price_jitter(rng));
    uint64_t price = mid_price + 1 + static_cast<uint64_t>(std::max<int64_t>(0, jitter));
    orders.emplace_back(OrderPrice{price}, makeTime(i), OrderQuantity{100},
                         OrderSide::Sell);
  }
  return orders;
}

}  // namespace

// -----------------------------------------------------------------------
// Insert-only path: every order rests, nothing crosses. Isolates the cost
// of OrderBook's insertion/bookkeeping with the match loop doing no work.
// -----------------------------------------------------------------------
static void BM_OrderBook_AddOrder_NoMatch(benchmark::State& state) {
  constexpr uint64_t kMid = 10'000;

  for (auto _ : state) {
    state.PauseTiming();
    OrderBook book;
    auto orders = makeRestingBuys(1, kMid);
    state.ResumeTiming();

    auto fills = book.addOrder(std::move(orders[0]));
    benchmark::DoNotOptimize(fills);
  }
}
BENCHMARK(BM_OrderBook_AddOrder_NoMatch);

// -----------------------------------------------------------------------
// Insert into a pre-populated book (no cross): measures cost as a function
// of resting book depth on the *opposite* side (shouldn't matter) and same
// side (tests whatever insertion structure OrderBook uses internally).
// -----------------------------------------------------------------------
static void BM_OrderBook_AddOrder_NoMatch_WithDepth(benchmark::State& state) {
  const auto depth = static_cast<std::size_t>(state.range(0));
  constexpr uint64_t kMid = 10'000;

  OrderBook book;
  for (auto& o : makeRestingBuys(depth, kMid)) {
    book.addOrder(std::move(o));
  }

  // Each iteration inserts one probe order, times that insert, then removes
  // it (untimed) before the next iteration. Without the removal, depth
  // grows by one per iteration — and since Benchmark auto-scales to
  // hundreds of thousands of iterations for a sub-microsecond op, every
  // run of this benchmark (regardless of the starting `depth` arg) ends up
  // converging toward roughly the same large size, which is what made the
  // original version look falsely flat across depth 10..10000.
  uint64_t churn_time = depth;
  std::mt19937_64 rng(99);
  std::normal_distribution<double> price_jitter(0.0, 25.0);

  for (auto _ : state) {
    auto jitter = static_cast<int64_t>(price_jitter(rng));
    uint64_t price =
        kMid - 1 - static_cast<uint64_t>(std::max<int64_t>(0, jitter));
    Order probe{OrderPrice{price}, makeTime(churn_time++), OrderQuantity{100},
                OrderSide::Buy};
    const OrderId id = probe.id;

    auto fills = book.addOrder(std::move(probe));
    benchmark::DoNotOptimize(fills);

    state.PauseTiming();
    auto _ = book.removeOrder(id);
    state.ResumeTiming();
  }
}
BENCHMARK(BM_OrderBook_AddOrder_NoMatch_WithDepth)
    ->Arg(10)
    ->Arg(100)
    ->Arg(1000)
    ->Arg(10000);

// -----------------------------------------------------------------------
// Crossing path: pre-load resting sells, then send an aggressing buy that
// matches one resting order. Isolates match() cost.
//
// NOTE: relies on m_match_buy_aggressor / m_match_sell_aggressor being
// correct for both sides. See flagged predicate issue in OrderBook.hpp —
// if sell-side crossing is broken, this benchmark's "match" variant for
// sell aggressors will silently degrade into the no-match path.
// -----------------------------------------------------------------------
static void BM_OrderBook_AddOrder_SingleMatch(benchmark::State& state) {
  constexpr uint64_t kMid = 10'000;

  for (auto _ : state) {
    state.PauseTiming();
    OrderBook book;
    // One resting sell at the touch.
    auto resting = makeNonCrossingSells(1, kMid - 1);  // price = kMid
    book.addOrder(std::move(resting[0]));
    // Aggressing buy priced to cross it.
    Order aggressor{OrderPrice{kMid + 5}, makeTime(1), OrderQuantity{100},
                     OrderSide::Buy};
    state.ResumeTiming();

    auto fills = book.addOrder(std::move(aggressor));
    benchmark::DoNotOptimize(fills);
  }
}
BENCHMARK(BM_OrderBook_AddOrder_SingleMatch);

// -----------------------------------------------------------------------
// Cancel path.
// -----------------------------------------------------------------------
static void BM_OrderBook_RemoveOrder(benchmark::State& state) {
  const auto depth = static_cast<std::size_t>(state.range(0));
  constexpr uint64_t kMid = 10'000;

  OrderBook book;
  // Static resting depth — held constant, never removed, just there so
  // removeOrder has to operate against a book of this size.
  for (auto& o : makeRestingBuys(depth, kMid)) {
    book.addOrder(std::move(o));
  }

  // One "churn" order is added (untimed) and then removed (timed) each
  // iteration. This avoids ever running out of ids — Benchmark picks the
  // iteration count dynamically and will happily run far more than `depth`
  // iterations, which is what caused the original version to error out.
  uint64_t churn_time = depth;
  for (auto _ : state) {
    state.PauseTiming();
    Order churn{OrderPrice{kMid - 1}, makeTime(churn_time++),
                OrderQuantity{100}, OrderSide::Buy};
    const OrderId id = churn.id;
    book.addOrder(std::move(churn));
    state.ResumeTiming();

    auto removed = book.removeOrder(id);
    benchmark::DoNotOptimize(removed);
  }
}
BENCHMARK(BM_OrderBook_RemoveOrder)->Arg(1000)->Arg(10000);