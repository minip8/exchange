/*
OrderBook microbenchmarks.

Read `BenchSupport.hpp` first — it documents the two fixture patterns used
throughout and why a per-iteration `PauseTiming()` is not one of them.

Build and run Release only; a sanitized number here is meaningless:

    cmake --build --preset bench
    ./build/release/bench/google/exchange_bench --benchmark_filter='BM_Add.*'
*/
#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "BenchSupport.hpp"
#include "engine/Order.hpp"
#include "engine/OrderBook.hpp"
#include "types/OrderId.hpp"
#include "types/OrderPrice.hpp"
#include "types/OrderQuantity.hpp"
#include "types/OrderSide.hpp"

using namespace Exchange::Bench;
using namespace Exchange::Engine;
using namespace Exchange::Types;

namespace {

// Batch size for the Pattern B benchmarks. Large enough that the pause costs
// under ~10 ns/op, small enough that a batch does not materially move the depth
// axis it is being measured against.
constexpr std::size_t kMatchBatch{32};

}  // namespace

// ---------------------------------------------------------------------
// Add + remove round trip on an empty book: level created, order pushed,
// order found, level erased. The floor cost of touching the book at all.
// Pattern A — the book is empty again at the end of every iteration, so
// there is no fixture to rebuild and no timer manipulation anywhere.
// ---------------------------------------------------------------------
static void BM_AddRemove_EmptyBook(benchmark::State& state) {
  OrderBook book{makeOrderBook()};
  uint64_t seq{0};

  for (auto _ : state) {
    Order order{OrderPrice{kBestBid}, seqTime(++seq), OrderQuantity{100},
                OrderSide::Buy};
    const OrderId id{order.id};
    benchmark::DoNotOptimize(book.addOrder(std::move(order)));
    benchmark::DoNotOptimize(book.removeOrder(id));
  }
}
BENCHMARK(BM_AddRemove_EmptyBook);

// ---------------------------------------------------------------------
// Add + remove a level at the *worst* end of a book `depth` levels deep.
//
// This is the O(depth) case by construction. `priceLevelIteratorImpl` scans
// from `rbegin()` — the best price outwards — so a worst-priced order walks
// every level, and the resulting `vector::insert` lands at `begin()` and
// shifts the whole vector. The paired erase costs the same shift back.
//
// Read this against BM_AddRemove_BestPrice_AtDepth below: the two curves
// together are the measured justification for the worst-first level ordering.
// ---------------------------------------------------------------------
static void BM_AddRemove_WorstPrice_AtDepth(benchmark::State& state) {
  const auto depth = static_cast<std::size_t>(state.range(0));
  uint64_t seq{0};
  OrderBook book = makeBook(seq, depth);

  // One tick below the worst resting bid, so the level never already exists.
  const OrderPrice price{kBestBid - depth};

  for (auto _ : state) {
    Order order{price, seqTime(++seq), OrderQuantity{100}, OrderSide::Buy};
    const OrderId id{order.id};
    benchmark::DoNotOptimize(book.addOrder(std::move(order)));
    benchmark::DoNotOptimize(book.removeOrder(id));
  }
  state.SetComplexityN(static_cast<int64_t>(depth));
}
BENCHMARK(BM_AddRemove_WorstPrice_AtDepth)
    ->RangeMultiplier(4)
    ->Range(1, 4096)
    ->Complexity();

// ---------------------------------------------------------------------
// Add + remove a new *best* level on a book `depth` levels deep.
//
// Expected to be flat in depth: the scan from `rbegin()` hits on its first
// comparison and the insert lands at `end()`, moving no element. This is the
// case the book's layout is designed for — activity concentrates at the top —
// and until now it was the one case the suite did not measure.
// ---------------------------------------------------------------------
static void BM_AddRemove_BestPrice_AtDepth(benchmark::State& state) {
  const auto depth = static_cast<std::size_t>(state.range(0));
  uint64_t seq{0};
  OrderBook book = makeBook(seq, depth);

  // kMid is the free tick inside the spread (see BenchSupport.hpp), so this is
  // a new best bid that creates a level and does not cross the best offer.
  const OrderPrice price{kMid};

  for (auto _ : state) {
    Order order{price, seqTime(++seq), OrderQuantity{100}, OrderSide::Buy};
    const OrderId id{order.id};
    benchmark::DoNotOptimize(book.addOrder(std::move(order)));
    benchmark::DoNotOptimize(book.removeOrder(id));
  }
  state.SetComplexityN(static_cast<int64_t>(depth));
}
BENCHMARK(BM_AddRemove_BestPrice_AtDepth)
    ->RangeMultiplier(4)
    ->Range(1, 4096)
    ->Complexity();

// ---------------------------------------------------------------------
// Add + remove within an existing level holding `n` orders.
//
// `PriceLevel::orders` is a vector and `removeOrder` locates the order with a
// linear `std::ranges::find` over it, so this is O(n) in orders-per-level —
// an axis every other fixture in this suite hides by resting exactly one
// order per level. It is also precisely the axis cb5c9ca changed.
// ---------------------------------------------------------------------
static void BM_AddRemove_WithinLevel(benchmark::State& state) {
  const auto orders_per_level = static_cast<std::size_t>(state.range(0));
  uint64_t seq{0};
  // A single level per side: the level scan is O(1) so what is left is the
  // intra-level cost.
  OrderBook book = makeBook(seq, 1, orders_per_level);

  for (auto _ : state) {
    // kBestBid is the one existing bid level, so this is a push_back into a
    // level already `orders_per_level` deep — no level is created or erased.
    Order order{OrderPrice{kBestBid}, seqTime(++seq), OrderQuantity{100},
                OrderSide::Buy};
    const OrderId id{order.id};
    benchmark::DoNotOptimize(book.addOrder(std::move(order)));
    benchmark::DoNotOptimize(book.removeOrder(id));
  }
  state.SetComplexityN(static_cast<int64_t>(orders_per_level));
}
BENCHMARK(BM_AddRemove_WithinLevel)
    ->RangeMultiplier(4)
    ->Range(1, 4096)
    ->Complexity();

// ---------------------------------------------------------------------
// Aggressive order that fully consumes one resting order at the best level.
// Pattern B: matching is destructive, so the book is rebuilt — in the paused
// region, where the previous book's destructor also lands.
// ---------------------------------------------------------------------
static void BM_AddOrder_Match_TopOfBook(benchmark::State& state) {
  uint64_t seq{0};
  std::optional<OrderBook> book;

  while (state.KeepRunningBatch(static_cast<int64_t>(kMatchBatch))) {
    state.PauseTiming();
    // Best offer level holds one order per aggressor in the batch.
    book = makeSellBook(seq, 10, kMatchBatch);
    state.ResumeTiming();

    for (std::size_t i = 0; i < kMatchBatch; ++i) {
      benchmark::DoNotOptimize(
          book->addOrder(Order{OrderPrice{kBestAsk}, seqTime(++seq),
                               OrderQuantity{100}, OrderSide::Buy}));
    }
  }
}
BENCHMARK(BM_AddOrder_Match_TopOfBook);

// ---------------------------------------------------------------------
// Aggressive order that sweeps `n` price levels before fully filling.
// Stresses the level-walking loop in `match` and its suffix erase, rather
// than the single-level match path above.
// ---------------------------------------------------------------------
static void BM_AddOrder_Match_SweepLevels(benchmark::State& state) {
  const auto levels_to_sweep = static_cast<std::size_t>(state.range(0));
  // Deep sweeps rebuild a large book per batch; keep the paused rebuild from
  // dominating the suite's wall-clock time.
  const std::size_t batch{levels_to_sweep >= 64 ? 4 : kMatchBatch};

  uint64_t seq{0};
  std::optional<OrderBook> book;

  while (state.KeepRunningBatch(static_cast<int64_t>(batch))) {
    state.PauseTiming();
    // Enough levels for every aggressor in the batch to sweep its own run.
    book = makeSellBook(seq, levels_to_sweep * batch);
    state.ResumeTiming();

    for (std::size_t i = 0; i < batch; ++i) {
      // Priced through the whole book and sized to consume exactly
      // `levels_to_sweep` levels of 100, so nothing rests.
      benchmark::DoNotOptimize(book->addOrder(
          Order{OrderPrice{kBestAsk + levels_to_sweep * batch}, seqTime(++seq),
                OrderQuantity{100 * levels_to_sweep}, OrderSide::Buy}));
    }
  }
  state.SetComplexityN(static_cast<int64_t>(levels_to_sweep));
}
BENCHMARK(BM_AddOrder_Match_SweepLevels)
    ->RangeMultiplier(2)
    ->Range(1, 256)
    ->Complexity();

// ---------------------------------------------------------------------
// Aggressive order that consumes `n` orders resting at a *single* price.
// This is `match`'s inner loop plus its `erase(begin, begin + n)` front-erase
// of the level vector — the other half of the intra-level cost, and the other
// half of what cb5c9ca was trying to change.
// ---------------------------------------------------------------------
static void BM_AddOrder_Match_WithinLevel(benchmark::State& state) {
  const auto orders_to_consume = static_cast<std::size_t>(state.range(0));
  const std::size_t batch{orders_to_consume >= 64 ? 4 : kMatchBatch};

  uint64_t seq{0};
  std::optional<OrderBook> book;

  while (state.KeepRunningBatch(static_cast<int64_t>(batch))) {
    state.PauseTiming();
    // One level, deep enough to feed every aggressor in the batch.
    book = makeSellBook(seq, 1, orders_to_consume * batch, 1);
    state.ResumeTiming();

    for (std::size_t i = 0; i < batch; ++i) {
      benchmark::DoNotOptimize(book->addOrder(
          Order{OrderPrice{kBestAsk}, seqTime(++seq),
                OrderQuantity{orders_to_consume}, OrderSide::Buy}));
    }
  }
  state.SetComplexityN(static_cast<int64_t>(orders_to_consume));
}
BENCHMARK(BM_AddOrder_Match_WithinLevel)
    ->RangeMultiplier(2)
    ->Range(1, 256)
    ->Complexity();

// ---------------------------------------------------------------------
// modifyOrder — remove then re-add, which loses time priority by design.
//
// Pattern A without needing an explicit inverse: re-adding puts the order at
// the back of the same level, so book *shape* is invariant and the same id can
// be modified indefinitely. The level holds two orders so that removing one
// does not erase the level and change the shape after all.
// ---------------------------------------------------------------------
static void BM_ModifyOrder(benchmark::State& state) {
  uint64_t seq{0};
  OrderBook book = makeBook(seq, 64, 2);

  const auto ids = restingBuyIds(book);
  const OrderId target{ids.front()};  // best level

  for (auto _ : state) {
    benchmark::DoNotOptimize(book.modifyOrder(target));
  }
}
BENCHMARK(BM_ModifyOrder);

// ---------------------------------------------------------------------
// Sustained mixed workload.
//
// The operation script is generated once, before the loop, and replayed
// verbatim: no RNG and no clock inside the timed region, and every replay does
// bit-identical work, which is what keeps this usable as a trend metric.
//
// Unlike the version this replaces, the flow genuinely crosses (aggressors are
// priced through the spread) and genuinely cancels, and the add/remove mix is
// chosen so book depth stays bounded — without which "steady state" and
// `SetItemsProcessed` both mean nothing. The dry run below asserts the
// crossing actually happens, so a future edit cannot quietly turn this back
// into an insert-only benchmark.
// ---------------------------------------------------------------------
namespace {

enum class Op : uint8_t { Add, Cancel, Aggress };

struct ScriptedOp {
  Op op;
  OrderSide side;
  OrderPrice::T price;
  OrderQuantity::T quantity;
  uint32_t cancel_pick;
};

constexpr std::size_t kScriptLength{1000};
constexpr std::size_t kSeedDepth{50};
constexpr std::size_t kSeedOrdersPerLevel{4};

// Ticks away from mid, concentrated at the top of book — the distribution the
// level scan's direction is chosen for.
uint64_t ticksFromMid(std::mt19937_64& gen) {
  std::geometric_distribution<uint64_t> dist(0.35);
  return std::min<uint64_t>(dist(gen), 19);
}

std::vector<ScriptedOp> makeScript() {
  auto gen = benchRng();
  // 45 / 40 / 15. Adds rest one order, cancels remove one, and an aggressor
  // removes ~0-2 — so removals roughly balance adds and depth stays put.
  std::discrete_distribution<int> op_dist({45, 40, 15});
  std::uniform_int_distribution<int> side_dist(0, 1);
  std::uniform_int_distribution<uint64_t> aggress_qty(20, 250);
  std::uniform_int_distribution<uint32_t> pick(0, 1u << 30);

  std::vector<ScriptedOp> script;
  script.reserve(kScriptLength);
  for (std::size_t i = 0; i < kScriptLength; ++i) {
    const auto op = static_cast<Op>(op_dist(gen));
    const auto side = side_dist(gen) == 0 ? OrderSide::Buy : OrderSide::Sell;
    const uint64_t ticks = ticksFromMid(gen);

    OrderPrice::T price{};
    OrderQuantity::T quantity{100};
    switch (op) {
      case Op::Add:
        // Rests: on the passive side of the spread. A buy at `ticks == 0`
        // lands on the free mid tick, which is a new best bid and still does
        // not cross.
        price = side == OrderSide::Buy ? kMid - ticks : kBestAsk + ticks;
        break;
      case Op::Aggress:
        // Crosses: priced at or through the opposite top of book.
        price = side == OrderSide::Buy ? kBestAsk + ticks : kBestBid - ticks;
        quantity = aggress_qty(gen);
        break;
      case Op::Cancel:
        break;
    }
    script.push_back({op, side, price, quantity, pick(gen)});
  }
  return script;
}

// Replays the script, returning the number of fills generated. Used both to
// drive the benchmark and — outside the timed region — to prove the flow
// crosses and that depth stays bounded.
std::size_t replayScript(OrderBook& book, const std::vector<ScriptedOp>& script,
                         std::vector<OrderId>& resting, uint64_t& seq) {
  std::size_t fills{0};
  for (const ScriptedOp& op : script) {
    switch (op.op) {
      case Op::Add:
      case Op::Aggress: {
        Order order{OrderPrice{op.price}, seqTime(++seq),
                    OrderQuantity{op.quantity}, op.side};
        const OrderId id{order.id};
        auto f = book.addOrder(std::move(order));
        fills += f.size();
        if (book.contains(id)) resting.push_back(id);
      } break;
      case Op::Cancel: {
        if (resting.empty()) break;
        const std::size_t idx{op.cancel_pick % resting.size()};
        // A cancel may miss: an aggressor can have consumed the order first.
        // That is the real behaviour of a live venue and is left in.
        benchmark::DoNotOptimize(book.removeOrder(resting[idx]));
        resting[idx] = resting.back();
        resting.pop_back();
      } break;
    }
  }
  return fills;
}

}  // namespace

static void BM_MixedWorkload_SteadyState(benchmark::State& state) {
  const std::vector<ScriptedOp> script = makeScript();

  // Dry run, outside the timed region: proves the flow crosses and reports
  // what the book looks like at the end of a replay.
  {
    uint64_t seq{0};
    OrderBook probe = makeBook(seq, kSeedDepth, kSeedOrdersPerLevel);
    std::vector<OrderId> resting = restingBuyIds(probe);
    const std::size_t fills = replayScript(probe, script, resting, seq);
    if (fills == 0) {
      state.SkipWithError(
          "mixed workload generated no fills — the flow is not crossing");
      return;
    }
    state.SetLabel("fills=" + std::to_string(fills) + " end_depth=" +
                   std::to_string(probe.buys().size() + probe.sells().size()));
  }

  uint64_t seq{0};
  std::optional<OrderBook> book;
  std::vector<OrderId> resting;

  while (state.KeepRunningBatch(static_cast<int64_t>(script.size()))) {
    state.PauseTiming();
    book = makeBook(seq, kSeedDepth, kSeedOrdersPerLevel);
    resting = restingBuyIds(*book);
    state.ResumeTiming();

    benchmark::DoNotOptimize(replayScript(*book, script, resting, seq));
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MixedWorkload_SteadyState);
