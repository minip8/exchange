/*
MatchingEngine microbenchmarks.

Everything here is a routing question: the engine is three `unordered_map`s in
front of `OrderBook`, so every one of these should be flat in the number of
books. Anything that grows with `n_books` is either a real defect or — as was
the case before this rewrite — a fixture being measured instead of an
operation. Read `BenchSupport.hpp` for the two fixture patterns.
*/
#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "BenchSupport.hpp"
#include "engine/MatchingEngine.hpp"
#include "engine/Order.hpp"
#include "engine/OrderBook.hpp"
#include "types/OrderBookId.hpp"
#include "types/OrderId.hpp"
#include "types/OrderPrice.hpp"
#include "types/OrderQuantity.hpp"
#include "types/OrderSide.hpp"
#include "types/Symbol.hpp"

using namespace Exchange::Bench;
using namespace Exchange::Engine;
using namespace Exchange::Types;

namespace {

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

SeededEngine makeEngine(uint64_t& seq, std::size_t n_books) {
  SeededEngine seeded;
  seeded.book_ids.reserve(n_books);
  seeded.symbols.reserve(n_books);
  seeded.resting_order_ids.reserve(n_books);

  for (std::size_t i = 0; i < n_books; ++i) {
    const auto symbol = symbolFor(i);
    // .value() so a registration failure is loud rather than silently
    // benchmarking an empty engine.
    const auto book_id = seeded.engine.addOrderBook(OrderBook{symbol}).value();
    seeded.book_ids.push_back(book_id);
    seeded.symbols.push_back(symbol);

    Order resting{OrderPrice{kBestBid}, seqTime(++seq), OrderQuantity{100},
                  OrderSide::Buy};
    seeded.resting_order_ids.push_back(resting.id);
    auto _ = seeded.engine.addOrder(book_id, std::move(resting));
  }
  return seeded;
}

/*
A pre-generated sequence of book indices.

The picks are drawn *before* the loop for the same reason the clock is: a
`uniform_int_distribution` over an `mt19937_64` costs ~10-15 ns, which would be
three times the operation in the lookup benchmarks below. A power-of-two length
keeps the wrap a mask rather than a division.
*/
std::vector<std::size_t> makePicks(std::size_t n_books) {
  constexpr std::size_t kPicks{4096};
  auto gen = benchRng();
  std::uniform_int_distribution<std::size_t> dist(0, n_books - 1);
  std::vector<std::size_t> picks(kPicks);
  for (auto& p : picks) p = dist(gen);
  return picks;
}

// Batch size for the Pattern B benchmarks here. The paused rebuild is
// O(n_books) and dominates wall-clock at the top of the sweep, so this is
// deliberately larger than the OrderBook suite's.
constexpr std::size_t kEngineBatch{256};

}  // namespace

// ---------------------------------------------------------------------
// getOrderBook(OrderBookId) — one hash lookup. The routing cost floor.
// ---------------------------------------------------------------------
static void BM_GetOrderBook_ById(benchmark::State& state) {
  const auto n_books = static_cast<std::size_t>(state.range(0));
  uint64_t seq{0};
  auto seeded = makeEngine(seq, n_books);
  const auto picks = makePicks(n_books);
  std::size_t i{0};

  for (auto _ : state) {
    const auto& book_id = seeded.book_ids[picks[i++ & (picks.size() - 1)]];
    benchmark::DoNotOptimize(seeded.engine.getOrderBook(book_id));
  }
  state.SetComplexityN(static_cast<int64_t>(n_books));
}
BENCHMARK(BM_GetOrderBook_ById)
    ->RangeMultiplier(8)
    ->Range(1, 8192)
    ->Complexity();

// ---------------------------------------------------------------------
// getOrderBook(OrderId) — OrderId -> OrderBookId, then OrderBookId ->
// OrderBook. Two hash lookups; the delta against the above is the cost of the
// indirection layer itself.
// ---------------------------------------------------------------------
static void BM_GetOrderBook_ByOrderId(benchmark::State& state) {
  const auto n_books = static_cast<std::size_t>(state.range(0));
  uint64_t seq{0};
  auto seeded = makeEngine(seq, n_books);
  const auto picks = makePicks(n_books);
  std::size_t i{0};

  for (auto _ : state) {
    const auto& order_id =
        seeded.resting_order_ids[picks[i++ & (picks.size() - 1)]];
    benchmark::DoNotOptimize(seeded.engine.getOrderBook(order_id));
  }
  state.SetComplexityN(static_cast<int64_t>(n_books));
}
BENCHMARK(BM_GetOrderBook_ByOrderId)
    ->RangeMultiplier(8)
    ->Range(1, 8192)
    ->Complexity();

// ---------------------------------------------------------------------
// getOrderBook(Symbol) — same two-lookup shape as the OrderId path, so the
// delta between them isolates hashing an 8-byte inline Symbol against hashing
// a uint64 id.
// ---------------------------------------------------------------------
static void BM_GetOrderBook_BySymbol(benchmark::State& state) {
  const auto n_books = static_cast<std::size_t>(state.range(0));
  uint64_t seq{0};
  auto seeded = makeEngine(seq, n_books);
  const auto picks = makePicks(n_books);
  std::size_t i{0};

  for (auto _ : state) {
    const auto& symbol = seeded.symbols[picks[i++ & (picks.size() - 1)]];
    benchmark::DoNotOptimize(seeded.engine.getOrderBook(symbol));
  }
  state.SetComplexityN(static_cast<int64_t>(n_books));
}
BENCHMARK(BM_GetOrderBook_BySymbol)
    ->RangeMultiplier(8)
    ->Range(1, 8192)
    ->Complexity();

// ---------------------------------------------------------------------
// addOrder routed through the engine, versus calling it on an OrderBook
// directly (bench_orderbook.cpp). The difference is the routing layer.
//
// Pattern A: each add is paired with the matching remove, so the target book
// keeps exactly one resting order and the engine's OrderId index keeps exactly
// one entry per book. No rebuild, and — the point of the sweep — nothing that
// scales with `n_books` happens inside the timed region.
// ---------------------------------------------------------------------
static void BM_Engine_AddOrder_Routed(benchmark::State& state) {
  const auto n_books = static_cast<std::size_t>(state.range(0));
  uint64_t seq{0};
  auto seeded = makeEngine(seq, n_books);
  const auto picks = makePicks(n_books);
  std::size_t i{0};

  for (auto _ : state) {
    const auto& book_id = seeded.book_ids[picks[i++ & (picks.size() - 1)]];
    Order order{OrderPrice{kBestBid}, seqTime(++seq), OrderQuantity{50},
                OrderSide::Buy};
    const OrderId id{order.id};
    benchmark::DoNotOptimize(seeded.engine.addOrder(book_id, std::move(order)));
    benchmark::DoNotOptimize(seeded.engine.removeOrder(id));
  }
  state.SetComplexityN(static_cast<int64_t>(n_books));
}
BENCHMARK(BM_Engine_AddOrder_Routed)
    ->RangeMultiplier(8)
    ->Range(1, 8192)
    ->Complexity();

// ---------------------------------------------------------------------
// removeOrder — OrderId -> OrderBookId -> OrderBook -> per-level erase, plus
// the index erase. Pattern A, re-adding the order it just removed.
// ---------------------------------------------------------------------
static void BM_Engine_RemoveOrder(benchmark::State& state) {
  const auto n_books = static_cast<std::size_t>(state.range(0));
  uint64_t seq{0};
  auto seeded = makeEngine(seq, n_books);
  const auto picks = makePicks(n_books);
  std::size_t i{0};

  for (auto _ : state) {
    const std::size_t pick{picks[i++ & (picks.size() - 1)]};
    const auto& book_id = seeded.book_ids[pick];
    const auto order_id = seeded.resting_order_ids[pick];

    auto removed = seeded.engine.removeOrder(order_id);
    benchmark::DoNotOptimize(removed);
    // Put it back so the next pick of this book finds it resting again. The
    // re-add carries the order's original id, so the index returns to exactly
    // the state it was in.
    benchmark::DoNotOptimize(
        seeded.engine.addOrder(book_id, std::move(removed).value()));
  }
  state.SetComplexityN(static_cast<int64_t>(n_books));
}
BENCHMARK(BM_Engine_RemoveOrder)
    ->RangeMultiplier(8)
    ->Range(1, 8192)
    ->Complexity();

// ---------------------------------------------------------------------
// addOrderBook — registering an instrument. Off the hot path (this happens at
// listing time), but worth a curve: the question the benchmark exists to
// answer is whether it is accidentally O(existing book count), and the old
// version answered "yes" because it was timing its own fixture's destructor.
//
// Pattern B: the fixture cannot be undone — there is no removeOrderBook — so
// the engine is rebuilt in the paused region every `kEngineBatch`
// registrations.
// ---------------------------------------------------------------------
static void BM_Engine_AddOrderBook(benchmark::State& state) {
  const auto n_existing = static_cast<std::size_t>(state.range(0));
  // The paused rebuild is O(n_existing) and costs several times what one
  // registration does, so a fixed batch leaves the sweep spending ~80x its
  // measured time rebuilding. Scaling the batch with `n_existing` holds that
  // ratio flat. The map therefore grows by up to 2x across a batch — acceptable
  // for a question ("does this scale with existing book count?") whose answer
  // is a flat line either way.
  const std::size_t batch{std::max(kEngineBatch, n_existing)};
  uint64_t seq{0};
  std::optional<SeededEngine> seeded;

  while (state.KeepRunningBatch(static_cast<int64_t>(batch))) {
    state.PauseTiming();
    // Move-assign, so the previous engine's destructor runs here rather than
    // in the timed region.
    seeded = makeEngine(seq, n_existing);
    state.ResumeTiming();

    for (std::size_t i = 0; i < batch; ++i) {
      // Symbols outside [0, n_existing) so every registration succeeds.
      benchmark::DoNotOptimize(
          seeded->engine.addOrderBook(OrderBook{symbolFor(n_existing + i)}));
    }
  }
  state.SetComplexityN(static_cast<int64_t>(n_existing));
}
BENCHMARK(BM_Engine_AddOrderBook)
    ->RangeMultiplier(8)
    ->Range(1, 8192)
    ->Complexity();

// ---------------------------------------------------------------------
// Multi-book steady state: the mixed workload of bench_orderbook.cpp, routed
// through the engine and round-robined over `n_books` active instruments —
// a venue running many symbols at once rather than one book in isolation.
//
// The interesting axis is not the absolute number but how it degrades with
// book count, which is a cache-residency question: 5000 books do not fit in
// L2 the way one does.
// ---------------------------------------------------------------------
namespace {

struct RoutedOp {
  std::size_t book;
  OrderPrice::T price;
  OrderQuantity::T quantity;
  OrderSide side;
  bool cancel;
};

constexpr std::size_t kRoutedScriptLength{1000};
// Replays per rebuild. `makeEngine(5000)` costs an order of magnitude more than
// the 1000 operations it sets up, so the paused rebuild would otherwise
// dominate the suite's wall-clock time. Adds and cancels are balanced, so the
// resting set does not drift across replays.
constexpr std::size_t kRoutedReplays{8};

std::vector<RoutedOp> makeRoutedScript(std::size_t n_books) {
  auto gen = benchRng();
  std::uniform_int_distribution<std::size_t> book_dist(0, n_books - 1);
  std::discrete_distribution<int> op_dist({50, 50});  // add, cancel
  std::uniform_int_distribution<int> side_dist(0, 1);
  std::geometric_distribution<uint64_t> tick_dist(0.35);

  std::vector<RoutedOp> script;
  script.reserve(kRoutedScriptLength);
  for (std::size_t i = 0; i < kRoutedScriptLength; ++i) {
    const auto side = side_dist(gen) == 0 ? OrderSide::Buy : OrderSide::Sell;
    const uint64_t ticks = std::min<uint64_t>(tick_dist(gen), 19);
    script.push_back({book_dist(gen),
                      side == OrderSide::Buy ? kMid - ticks : kBestAsk + ticks,
                      100, side, op_dist(gen) == 1});
  }
  return script;
}

}  // namespace

static void BM_Engine_MultiBook_SteadyState(benchmark::State& state) {
  const auto n_books = static_cast<std::size_t>(state.range(0));
  const auto script = makeRoutedScript(n_books);

  uint64_t seq{0};
  std::optional<SeededEngine> seeded;
  std::vector<OrderId> resting;

  while (state.KeepRunningBatch(
      static_cast<int64_t>(script.size() * kRoutedReplays))) {
    state.PauseTiming();
    // Move-assign so the previous engine's destructor runs here, paused.
    seeded = makeEngine(seq, n_books);
    resting = seeded->resting_order_ids;
    state.ResumeTiming();

    for (std::size_t replay = 0; replay < kRoutedReplays; ++replay) {
      for (const RoutedOp& op : script) {
        if (op.cancel && !resting.empty()) {
          const std::size_t idx{op.book % resting.size()};
          benchmark::DoNotOptimize(seeded->engine.removeOrder(resting[idx]));
          resting[idx] = resting.back();
          resting.pop_back();
          continue;
        }
        Order order{OrderPrice{op.price}, seqTime(++seq),
                    OrderQuantity{op.quantity}, op.side};
        const OrderId id{order.id};
        benchmark::DoNotOptimize(seeded->engine.addOrder(
            seeded->book_ids[op.book], std::move(order)));
        resting.push_back(id);
      }
    }
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Engine_MultiBook_SteadyState)->Arg(1)->Arg(100)->Arg(5000);
