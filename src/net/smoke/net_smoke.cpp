/*
net_smoke — the correctness harness for the networking layer.

There is no unit-test framework here, and none should be added: the repo's
`check()`-not-`assert` convention already works (Release defines NDEBUG,
which would compile assertions away) and stays dependency-free.

This links net_gateway only — no Boost, no sockets — which is exactly why the
EventSink seam exists. Everything the matching thread does is driven
synchronously through a VectorEventSink and asserted on.

Run it under BOTH trees. That pair is the substitute for a unit-test suite,
and for this class of bug it is stronger than most suites would be:

    cmake --build --preset smoke      && ./build/debug/src/net/net_smoke
    cmake --build --preset smoke-tsan && ./build/tsan/src/net/net_smoke
*/
#include <atomic>
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <cstring>
#include <memory>
#include <optional>
#include <print>
#include <random>
#include <string_view>
#include <thread>
#include <vector>

#include "net/core/Command.hpp"
#include "net/core/Event.hpp"
#include "net/core/RejectCode.hpp"
#include "net/core/SpscRing.hpp"
#include "net/gateway/EventSink.hpp"
#include "net/gateway/MatchingLoop.hpp"
#include "net/io/Server.hpp"
#include "net/wire/BinaryProtocol.hpp"
#include "net/wire/MessageNames.hpp"
#include "types/Symbol.hpp"

using namespace Exchange::Net;
using Exchange::Types::Symbol;

namespace {
int g_failures{0};

void check(bool condition, std::string_view what) {
  if (!condition) {
    std::println("FAIL: {}", what);
    ++g_failures;
  }
}

// _GLIBCXX_DEBUG and the sanitizers make the ring stress test 50-100x
// slower. Keep the shape of the test identical and shrink the count, so the
// same code runs in every config.
#if defined(_GLIBCXX_DEBUG) || defined(__SANITIZE_ADDRESS__) || \
    defined(__SANITIZE_THREAD__)
constexpr uint64_t kRingStressItems{2'000'000};
#else
constexpr uint64_t kRingStressItems{10'000'000};
#endif

// ===========================================================================
// Layer 1 — ring invariants
// ===========================================================================

void testRingBasics() {
  SpscRing<Command, 4> ring{};
  Command out{};

  check(!ring.tryPop(out), "a fresh ring is empty");
  check(ring.freeSlots() == 4, "a fresh ring has every slot free");

  for (uint64_t i{0}; i < 4; ++i) {
    Command command{};
    command.recv_ts_ns = i;
    check(ring.tryPush(command), "pushing into a non-full ring succeeds");
  }
  // No sacrificed slot: monotonic never-wrapped indices make full and empty
  // distinguishable without one.
  check(ring.freeSlots() == 0, "capacity is the full capacity");
  check(!ring.tryPush(Command{}), "pushing into a full ring fails");

  for (uint64_t i{0}; i < 4; ++i) {
    check(ring.tryPop(out), "popping a non-empty ring succeeds");
    check(out.recv_ts_ns == i, "items come out in FIFO order");
  }
  check(!ring.tryPop(out), "a drained ring is empty again");

  // Drive the masking well past one lap to prove the indices wrap cleanly.
  for (uint64_t i{0}; i < 1000; ++i) {
    Command command{};
    command.recv_ts_ns = i;
    check(ring.tryPush(command), "push across the wrap boundary");
    check(ring.tryPop(out), "pop across the wrap boundary");
    check(out.recv_ts_ns == i, "wrapped items keep their identity");
  }
}

void testRingBatch() {
  SpscRing<Event, 8> ring{};

  std::vector<Event> in(5);
  for (std::size_t i{0}; i < in.size(); ++i) {
    in[i].payload.raw[0] = i + 1;
  }
  check(ring.tryPushBatch(in), "a batch that fits is accepted");

  std::vector<Event> too_big(6);
  // All-or-nothing: 5 in, 8 capacity, so 6 cannot fit and must not partially
  // land. That atomicity is what lets a snapshot cross as a unit.
  check(!ring.tryPushBatch(too_big), "an oversized batch is rejected whole");
  check(ring.sizeApprox() == 5, "a rejected batch leaves nothing behind");

  std::vector<Event> out(16);
  const std::size_t popped{ring.tryPopBatch(out)};
  check(popped == 5, "the batch pops back out complete");
  for (std::size_t i{0}; i < popped; ++i) {
    check(out[i].payload.raw[0] == i + 1, "batched items keep their order");
  }
  check(ring.tryPopBatch(out) == 0, "an empty ring pops nothing");

  std::vector<Event> nothing{};
  check(ring.tryPushBatch(nothing), "an empty batch trivially succeeds");
}

/*
The test the tsan preset exists for.

A checksum rather than a per-item comparison, because a checksum catches
exactly the failures the memory ordering exists to prevent: a slot read
before the producer's release store makes it visible (stale value), or
overwritten before the consumer's release store said it was free (lost
value). Either shows up as a mismatched sum.
*/
void testRingThreaded() {
  auto ring{std::make_unique<SpscRing<Command, 1024>>()};
  std::atomic<bool> producer_done{false};

  const uint64_t expected{kRingStressItems * (kRingStressItems - 1) / 2};

  std::thread producer{[&] {
    for (uint64_t i{0}; i < kRingStressItems; ++i) {
      Command command{};
      command.recv_ts_ns = i;
      while (!ring->tryPush(command)) cpuPause();
    }
    producer_done.store(true, std::memory_order_release);
  }};

  uint64_t sum{0};
  uint64_t count{0};
  std::array<Command, 64> batch{};
  while (count < kRingStressItems) {
    const std::size_t n{ring->tryPopBatch(batch)};
    if (n == 0) {
      cpuPause();
      continue;
    }
    for (std::size_t i{0}; i < n; ++i) sum += batch[i].recv_ts_ns;
    count += n;
  }
  producer.join();

  check(producer_done.load(std::memory_order_acquire), "the producer ran");
  check(count == kRingStressItems, "every item crossed the ring exactly once");
  check(sum == expected, "no item was lost, duplicated, or read stale");
}

// ===========================================================================
// Layer 2 — scripted gateway scenarios
// ===========================================================================

constexpr uint32_t kSessionA{0x0100'0001};  // I/O thread 1, local session 1
constexpr uint32_t kSessionB{0x0100'0002};
constexpr uint32_t kTraderA{11};
constexpr uint32_t kTraderB{22};

// A gateway under test plus the sink that captures everything it emits.
struct Harness {
  VectorEventSink sink{};
  MatchingLoop loop{sink};

  void run(const Command& command) { loop.handle(command); }

  std::vector<Event> since(std::size_t mark) const {
    const auto& all{sink.events()};
    return std::vector<Event>{all.begin() + static_cast<std::ptrdiff_t>(mark),
                              all.end()};
  }
  std::size_t mark() const { return sink.events().size(); }
};

Command command(CommandType type, uint32_t session, uint32_t trader) {
  Command c{};
  c.type = type;
  c.session_id = session;
  c.trader_id = trader;
  c.recv_ts_ns = 1'700'000'000'000'000'000ull;
  return c;
}

Command logon(uint32_t session, uint32_t trader, bool cancel_on_disconnect) {
  Command c{command(CommandType::SessionOpened, session, trader)};
  if (cancel_on_disconnect) c.flags |= CommandFlags::kCancelOnDisconnect;
  return c;
}

Command createBook(uint32_t session, uint32_t trader, std::string_view ticker) {
  Command c{command(CommandType::CreateBook, session, trader)};
  c.symbol = Symbol::tryMake(ticker).value_or(Symbol{});
  return c;
}

Command newOrder(uint32_t session, uint32_t trader, uint64_t book_id, Side side,
                 uint64_t price, uint64_t quantity, uint64_t coid,
                 TimeInForce tif = TimeInForce::Gtc) {
  Command c{command(CommandType::NewOrder, session, trader)};
  c.book_id = book_id;
  c.side = side;
  c.price = price;
  c.quantity = quantity;
  c.client_order_id = coid;
  c.tif = tif;
  return c;
}

std::size_t countOf(const std::vector<Event>& events, EventType type) {
  std::size_t n{0};
  for (const Event& event : events) n += (event.type == type);
  return n;
}

// Returns a copy, not a pointer. Callers routinely pass `harness.since(mark)`
// directly, and a pointer into that temporary would dangle the moment the
// full expression ended — which is exactly what ASan caught the first time
// this file was written.
std::optional<Event> firstOf(const std::vector<Event>& events, EventType type) {
  for (const Event& event : events) {
    if (event.type == type) return event;
  }
  return std::nullopt;
}

// Logs both traders on and lists one book; returns its id.
uint64_t bootstrap(Harness& harness, std::string_view ticker = "NVDA") {
  harness.run(logon(kSessionA, kTraderA, false));
  harness.run(logon(kSessionB, kTraderB, false));
  const std::size_t mark{harness.mark()};
  harness.run(createBook(kSessionA, kTraderA, ticker));
  const std::optional<Event> ack{
      firstOf(harness.since(mark), EventType::CreateBookAck)};
  return !ack.has_value() ? 0 : ack->payload.book.book_id;
}

void testLogonAndBookAdmin() {
  Harness harness{};

  std::size_t mark{harness.mark()};
  harness.run(logon(kSessionA, kTraderA, false));
  auto events{harness.since(mark)};
  check(countOf(events, EventType::LogonAck) == 1, "logon acks once");
  // The listing rides in the same batch as the ack, so a client is never
  // logged on without knowing the symbol -> book_id mapping it needs.
  check(countOf(events, EventType::BookListEnd) == 1,
        "logon is followed by the book listing");
  check(events.back().flags & EventFlags::kEndOfBatch,
        "the last event of a batch is flagged");

  mark = harness.mark();
  harness.run(createBook(kSessionA, kTraderA, "NVDA"));
  events = harness.since(mark);
  const std::optional<Event> created{firstOf(events, EventType::CreateBookAck)};
  check(created.has_value() &&
            created->payload.book.reject_code == RejectCode::None,
        "creating a book succeeds");
  check(created.has_value() && created->payload.book.symbol.view() == "NVDA",
        "the ack names the instrument");

  mark = harness.mark();
  harness.run(createBook(kSessionA, kTraderA, "NVDA"));
  const std::optional<Event> duplicate{
      firstOf(harness.since(mark), EventType::CreateBookAck)};
  check(duplicate.has_value() &&
            duplicate->payload.book.reject_code == RejectCode::DuplicateSymbol,
        "relisting an instrument is rejected, not silently replaced");

  // Symbol's explicit constructor writes past the end on more than 8
  // characters — UB on untrusted input — so tryMake is the only path from
  // the wire, and it must refuse.
  check(!Symbol::tryMake("TOOLONGSYM").has_value(),
        "a 10-character ticker is refused by tryMake");
  mark = harness.mark();
  harness.run(createBook(kSessionA, kTraderA, "TOOLONGSYM"));
  const std::optional<Event> refused{
      firstOf(harness.since(mark), EventType::Reject)};
  check(refused.has_value() &&
            refused->payload.ack.reject_code == RejectCode::InvalidSymbol,
        "an over-long ticker never reaches the engine");

  mark = harness.mark();
  harness.run(command(CommandType::ListBooks, kSessionA, kTraderA));
  events = harness.since(mark);
  check(countOf(events, EventType::BookEntry) == 1, "one book is listed");
  const std::optional<Event> end{firstOf(events, EventType::BookListEnd)};
  check(end.has_value() && end->payload.book.count == 1,
        "the listing terminator carries the count");
}

void testOrderRestsAndValidation() {
  Harness harness{};
  const uint64_t book{bootstrap(harness)};

  std::size_t mark{harness.mark()};
  harness.run(newOrder(kSessionA, kTraderA, book, Side::Buy, 50, 100, 1));
  auto events{harness.since(mark)};
  const std::optional<Event> ack{firstOf(events, EventType::OrderAck)};
  check(ack.has_value() && ack->payload.ack.quantity == 100,
        "an unmatched order rests in full");
  check(ack.has_value() && ack->payload.ack.order_id != 0,
        "order ids never use 0, which is the wire sentinel");
  check(harness.loop.orders().size() == 1, "the resting order is tracked");
  check(countOf(events, EventType::ExecReport) == 0,
        "resting produces no fills");

  // A zero-quantity order silently vanishes inside the engine, so the client
  // would otherwise be acked for an order that does not exist.
  mark = harness.mark();
  harness.run(newOrder(kSessionA, kTraderA, book, Side::Buy, 50, 0, 2));
  const std::optional<Event> zero_qty{
      firstOf(harness.since(mark), EventType::Reject)};
  check(zero_qty.has_value() &&
            zero_qty->payload.ack.reject_code == RejectCode::InvalidQuantity,
        "zero quantity is rejected");

  // OrderPrice{0} would rest forever on the bid and cross everything on the
  // offer. Neither is something a client meant.
  mark = harness.mark();
  harness.run(newOrder(kSessionA, kTraderA, book, Side::Buy, 0, 100, 3));
  const std::optional<Event> zero_px{
      firstOf(harness.since(mark), EventType::Reject)};
  check(zero_px.has_value() &&
            zero_px->payload.ack.reject_code == RejectCode::InvalidPrice,
        "zero price is rejected");

  mark = harness.mark();
  harness.run(newOrder(kSessionA, kTraderA, book + 999, Side::Buy, 50, 10, 4));
  const std::optional<Event> bad_book{
      firstOf(harness.since(mark), EventType::Reject)};
  check(bad_book.has_value() &&
            bad_book->payload.ack.reject_code == RejectCode::UnknownBook,
        "an unknown book is rejected");

  // Client order ids are the client's dedup handle; reusing a live one would
  // make cancel-by-coid ambiguous.
  mark = harness.mark();
  harness.run(newOrder(kSessionA, kTraderA, book, Side::Buy, 49, 10, 1));
  const std::optional<Event> dup{
      firstOf(harness.since(mark), EventType::Reject)};
  check(dup.has_value() &&
            dup->payload.ack.reject_code == RejectCode::DuplicateClientOrderId,
        "a live client order id cannot be reused");
}

void testCrossingProducesTwoExecReports() {
  Harness harness{};
  const uint64_t book{bootstrap(harness)};

  harness.run(newOrder(kSessionA, kTraderA, book, Side::Sell, 50, 100, 1));

  const std::size_t mark{harness.mark()};
  harness.run(newOrder(kSessionB, kTraderB, book, Side::Buy, 50, 100, 1));
  const auto events{harness.since(mark)};

  // Fill carries no owner, so the gateway has to fan out two reports per
  // fill: one to the aggressor from the Command, one to the resting owner
  // looked up in the OrderStore.
  check(countOf(events, EventType::ExecReport) == 2,
        "one fill produces exactly two exec reports");

  std::size_t to_a{0};
  std::size_t to_b{0};
  bool aggressor_flagged{false};
  for (const Event& event : events) {
    if (event.type != EventType::ExecReport) continue;
    if (event.session_id == kSessionA) ++to_a;
    if (event.session_id == kSessionB) ++to_b;
    if (event.flags & EventFlags::kAggressor) {
      aggressor_flagged = true;
      check(event.session_id == kSessionB, "the taker is flagged aggressor");
    }
    check(event.payload.exec.price == 50,
          "fills print at the resting order's price");
    check(event.payload.exec.leaves == 0, "both orders are fully filled");
    check(event.flags & EventFlags::kFinal, "both orders are done");
  }
  check(to_a == 1 && to_b == 1, "each owner gets exactly one report");
  check(aggressor_flagged, "exactly the taker carries the aggressor flag");

  check(harness.loop.orders().size() == 0,
        "a fully filled order leaves the store, as it leaves the book");
}

void testCancelSemantics() {
  Harness harness{};
  const uint64_t book{bootstrap(harness)};

  // --- cancel of a live order, by id ---
  std::size_t mark{harness.mark()};
  harness.run(newOrder(kSessionA, kTraderA, book, Side::Buy, 50, 100, 1));
  const std::optional<Event> ack{
      firstOf(harness.since(mark), EventType::OrderAck)};
  const uint64_t live_id{!ack.has_value() ? 0 : ack->payload.ack.order_id};

  mark = harness.mark();
  Command cancel{command(CommandType::Cancel, kSessionA, kTraderA)};
  cancel.order_id = live_id;
  harness.run(cancel);
  const std::optional<Event> cancelled{
      firstOf(harness.since(mark), EventType::CancelAck)};
  check(cancelled.has_value() && cancelled->payload.ack.quantity == 100,
        "cancel returns the unfilled remainder");
  check(harness.loop.orders().size() == 0, "the cancelled order is gone");

  // --- cancel by client order id ---
  harness.run(newOrder(kSessionA, kTraderA, book, Side::Buy, 49, 10, 7));
  mark = harness.mark();
  Command by_coid{command(CommandType::Cancel, kSessionA, kTraderA)};
  by_coid.client_order_id = 7;  // order_id left 0: resolve through the coid
  harness.run(by_coid);
  check(firstOf(harness.since(mark), EventType::CancelAck).has_value(),
        "cancel resolves through (session, client_order_id)");

  // --- cancel of an order that was already fully filled ---
  mark = harness.mark();
  harness.run(newOrder(kSessionA, kTraderA, book, Side::Sell, 50, 100, 2));
  const std::optional<Event> resting{
      firstOf(harness.since(mark), EventType::OrderAck)};
  const uint64_t filled_id{
      !resting.has_value() ? 0 : resting->payload.ack.order_id};
  harness.run(newOrder(kSessionB, kTraderB, book, Side::Buy, 50, 100, 2));

  mark = harness.mark();
  Command stale{command(CommandType::Cancel, kSessionA, kTraderA)};
  stale.order_id = filled_id;
  harness.run(stale);
  const std::optional<Event> stale_reject{
      firstOf(harness.since(mark), EventType::Reject)};
  // EngineError::OrderNotFound is overloaded — genuinely unknown vs. already
  // filled — but from the client's side both mean "nothing left to cancel",
  // and the store answers without ever entering the engine.
  check(stale_reject.has_value() &&
            stale_reject->payload.ack.reject_code == RejectCode::UnknownOrder,
        "cancelling a fully filled order answers UnknownOrder");

  // --- cancel of somebody else's order ---
  mark = harness.mark();
  harness.run(newOrder(kSessionA, kTraderA, book, Side::Buy, 40, 100, 3));
  const std::optional<Event> mine{
      firstOf(harness.since(mark), EventType::OrderAck)};
  const uint64_t my_id{!mine.has_value() ? 0 : mine->payload.ack.order_id};

  mark = harness.mark();
  Command theft{command(CommandType::Cancel, kSessionB, kTraderB)};
  theft.order_id = my_id;
  harness.run(theft);
  const std::optional<Event> denied{
      firstOf(harness.since(mark), EventType::Reject)};
  // Must be indistinguishable from an unknown id, or the protocol becomes an
  // order-id enumeration oracle. NotYourOrder exists for the server log only.
  check(denied.has_value() &&
            denied->payload.ack.reject_code == RejectCode::UnknownOrder,
        "cancelling another trader's order is indistinguishable from unknown");
  check(harness.loop.orders().size() == 1, "and it is not actually cancelled");
}

void testAmendMintsANewId() {
  Harness harness{};
  const uint64_t book{bootstrap(harness)};

  std::size_t mark{harness.mark()};
  harness.run(newOrder(kSessionA, kTraderA, book, Side::Buy, 50, 100, 1));
  const std::optional<Event> ack{
      firstOf(harness.since(mark), EventType::OrderAck)};
  const uint64_t original{!ack.has_value() ? 0 : ack->payload.ack.order_id};

  mark = harness.mark();
  Command amend{command(CommandType::Amend, kSessionA, kTraderA)};
  amend.order_id = original;
  amend.price = 51;
  amend.quantity = 80;
  amend.client_order_id = 2;
  harness.run(amend);

  const std::optional<Event> amended{
      firstOf(harness.since(mark), EventType::AmendAck)};
  check(amended.has_value(), "amend acks");
  if (amended.has_value()) {
    // OrderBook::modifyOrder takes no new price or quantity — it re-adds the
    // identical order — so amend is remove + re-add under a new id. Reusing
    // the id after a partial fill would make the exec stream ambiguous.
    check(amended->payload.ack.order_id != original,
          "amend mints a new order id");
    check(amended->payload.ack.orig_order_id == original,
          "the ack carries the replaced id, FIX OrigClOrdID style");
    check(
        amended->payload.ack.price == 51 && amended->payload.ack.quantity == 80,
        "the amend takes effect");
  }
  check(harness.loop.orders().size() == 1, "exactly one order still rests");

  // The original id is dead: cancelling it must not resurrect anything.
  mark = harness.mark();
  Command cancel_old{command(CommandType::Cancel, kSessionA, kTraderA)};
  cancel_old.order_id = original;
  harness.run(cancel_old);
  check(firstOf(harness.since(mark), EventType::Reject).has_value(),
        "the replaced id is no longer cancellable");
}

void testIocAndMarketOrders() {
  Harness harness{};
  const uint64_t book{bootstrap(harness)};

  // --- IOC with nothing to trade against: the whole thing is pulled ---
  std::size_t mark{harness.mark()};
  harness.run(newOrder(kSessionA, kTraderA, book, Side::Buy, 50, 100, 1,
                       TimeInForce::Ioc));
  auto events{harness.since(mark)};
  const std::optional<Event> pulled{firstOf(events, EventType::CancelAck)};
  check(pulled.has_value() && pulled->payload.ack.quantity == 100,
        "an unfilled IOC residual is pulled back out of the book");
  check(pulled.has_value() && (pulled->flags & EventFlags::kFinal),
        "the pull is final");
  check(harness.loop.orders().size() == 0, "an IOC never rests");

  // --- IOC that partially fills ---
  harness.run(newOrder(kSessionA, kTraderA, book, Side::Sell, 50, 40, 2));
  mark = harness.mark();
  harness.run(newOrder(kSessionB, kTraderB, book, Side::Buy, 50, 100, 2,
                       TimeInForce::Ioc));
  events = harness.since(mark);
  check(countOf(events, EventType::ExecReport) == 2, "the 40 available fill");
  const std::optional<Event> residual{firstOf(events, EventType::CancelAck)};
  check(residual.has_value() && residual->payload.ack.quantity == 60,
        "the 60 that could not fill are pulled");
  check(harness.loop.orders().size() == 0, "nothing is left resting");

  // --- market order: a marketable limit synthesized at the gateway ---
  harness.run(newOrder(kSessionA, kTraderA, book, Side::Sell, 55, 30, 3));
  mark = harness.mark();
  Command market{newOrder(kSessionB, kTraderB, book, Side::Buy, 0, 30, 3)};
  market.flags |= CommandFlags::kMarket;
  harness.run(market);
  events = harness.since(mark);
  check(countOf(events, EventType::ExecReport) == 2,
        "a market buy sweeps the offer");
  const std::optional<Event> exec{firstOf(events, EventType::ExecReport)};
  check(exec.has_value() && exec->payload.exec.price == 55,
        "and prints at the resting price, not at the synthetic limit");
  check(harness.loop.orders().size() == 0,
        "a market order is IOC, so it never rests at UINT64_MAX");
}

/*
Market data: the snapshot/delta contract, end to end.

The claim being tested is the sequencing one from MarketDataPublisher.hpp:
because SubscribeMd goes through the same ordered path as every other
command, a subscriber's snapshot and the deltas that follow it are
contiguous, with no gap window.
*/
void testMarketData() {
  Harness harness{};
  const uint64_t book{bootstrap(harness)};

  std::size_t mark{harness.mark()};
  Command subscribe{command(CommandType::SubscribeMd, kSessionA, kTraderA)};
  subscribe.book_id = book;
  harness.run(subscribe);
  auto events{harness.since(mark)};
  check(countOf(events, EventType::SnapshotBegin) == 1 &&
            countOf(events, EventType::SnapshotEnd) == 1,
        "subscribing yields exactly one snapshot");
  const std::optional<Event> begin{firstOf(events, EventType::SnapshotBegin)};
  check(begin.has_value() && begin->payload.md.aggregate == 0,
        "the snapshot of an empty book has no levels");
  check(begin.has_value() && begin->payload.md.depth == 10,
        "the book's published depth is reported back");

  const uint64_t snapshot_seq{!begin.has_value() ? 0
                                                 : begin->payload.md.md_seq};

  // Every market-data message the subscriber sees from here must be exactly
  // one more than the last. That is the whole client-side gap check.
  uint64_t expected{snapshot_seq};
  auto trackTape{[&](const std::vector<Event>& batch) {
    for (const Event& event : batch) {
      if (event.session_id != kSessionA) continue;
      if (event.type != EventType::LevelUpdate &&
          event.type != EventType::TradePrint) {
        continue;
      }
      ++expected;
      check(event.payload.md.md_seq == expected,
            "market data arrives with no sequence gap");
    }
  }};

  mark = harness.mark();
  harness.run(newOrder(kSessionA, kTraderA, book, Side::Buy, 50, 100, 1));
  events = harness.since(mark);
  const std::optional<Event> level{firstOf(events, EventType::LevelUpdate)};
  check(level.has_value() && level->payload.md.price == 50 &&
            level->payload.md.quantity == 100 &&
            level->payload.md.level_side == Side::Buy,
        "a resting order appears as a level");
  trackTape(events);

  mark = harness.mark();
  harness.run(newOrder(kSessionA, kTraderA, book, Side::Buy, 50, 25, 2));
  events = harness.since(mark);
  const std::optional<Event> aggregated{
      firstOf(events, EventType::LevelUpdate)};
  check(aggregated.has_value() && aggregated->payload.md.quantity == 125,
        "a second order at the same price aggregates into the level");
  trackTape(events);

  mark = harness.mark();
  harness.run(newOrder(kSessionB, kTraderB, book, Side::Sell, 50, 125, 1));
  events = harness.since(mark);
  // Two resting orders sat on that level, so the sweep is two fills — and
  // the tape carries one print each, not one per command.
  check(countOf(events, EventType::TradePrint) == 2,
        "the tape carries one print per fill, not per order");
  const std::optional<Event> print{firstOf(events, EventType::TradePrint)};
  check(print.has_value() && print->side == Side::Sell,
        "the print names the aggressor's side");
  bool saw_delete{false};
  for (const Event& event : events) {
    if (event.type == EventType::LevelUpdate &&
        event.payload.md.quantity == 0) {
      saw_delete = true;
      check(event.payload.md.price == 50, "the emptied level is the one hit");
    }
  }
  check(saw_delete, "an emptied level is published as quantity 0, not omitted");
  trackTape(events);

  // Unsubscribing stops the feed.
  mark = harness.mark();
  Command unsubscribe{command(CommandType::UnsubscribeMd, kSessionA, kTraderA)};
  unsubscribe.book_id = book;
  harness.run(unsubscribe);
  harness.run(newOrder(kSessionA, kTraderA, book, Side::Buy, 45, 10, 5));
  events = harness.since(mark);
  check(countOf(events, EventType::LevelUpdate) == 0,
        "an unsubscribed session receives no further levels");

  // Re-subscribing restates everything, so a client that lost its place can
  // always recover with GetSnapshot.
  mark = harness.mark();
  harness.run(subscribe);
  events = harness.since(mark);
  const std::optional<Event> resnapshot{
      firstOf(events, EventType::SnapshotBegin)};
  check(resnapshot.has_value() && resnapshot->payload.md.aggregate == 1,
        "the resnapshot shows the level added while unsubscribed");
}

void testPositions() {
  Harness harness{};
  const uint64_t book{bootstrap(harness)};

  auto positionOf{[&](uint32_t trader) {
    return harness.loop.positions().find(trader,
                                         Exchange::Types::OrderBookId{book});
  }};

  // A sells 100 at 50 to B.
  harness.run(newOrder(kSessionA, kTraderA, book, Side::Sell, 50, 100, 1));
  harness.run(newOrder(kSessionB, kTraderB, book, Side::Buy, 50, 100, 1));

  const Position* a{positionOf(kTraderA)};
  const Position* b{positionOf(kTraderB)};
  check(a != nullptr && a->net_quantity == -100 && a->avg_cost == 50,
        "the seller is short 100 at 50");
  check(b != nullptr && b->net_quantity == 100 && b->avg_cost == 50,
        "the buyer is long 100 at 50");

  // B sells the lot back to A at 60: A covers a short at a loss, B closes a
  // long at a gain, and both end flat.
  harness.run(newOrder(kSessionB, kTraderB, book, Side::Sell, 60, 100, 2));
  harness.run(newOrder(kSessionA, kTraderA, book, Side::Buy, 60, 100, 2));

  a = positionOf(kTraderA);
  b = positionOf(kTraderB);
  check(a != nullptr && a->net_quantity == 0 && a->realized_pnl == -1000,
        "shorting at 50 and covering at 60 realizes -1000");
  check(b != nullptr && b->net_quantity == 0 && b->realized_pnl == 1000,
        "buying at 50 and selling at 60 realizes +1000");
  check(a != nullptr && a->unrealizedPnl() == 0,
        "a flat position has no unrealized P&L");
}

void testDisconnect() {
  // Default: resting orders survive a disconnect. That is what GTC means.
  {
    Harness harness{};
    const uint64_t book{bootstrap(harness)};
    harness.run(newOrder(kSessionA, kTraderA, book, Side::Buy, 50, 100, 1));
    harness.run(command(CommandType::SessionClosed, kSessionA, kTraderA));
    check(harness.loop.orders().size() == 1,
          "a plain disconnect leaves resting orders alone");
  }

  // Opt-in cancel-on-disconnect, which is the right default for an algo
  // client and the wrong one for a browser tab.
  {
    Harness harness{};
    harness.run(logon(kSessionA, kTraderA, true));
    harness.run(logon(kSessionB, kTraderB, false));
    std::size_t mark{harness.mark()};
    harness.run(createBook(kSessionA, kTraderA, "AMD"));
    const std::optional<Event> created{
        firstOf(harness.since(mark), EventType::CreateBookAck)};
    const uint64_t book{created.has_value() ? created->payload.book.book_id
                                            : 0};

    harness.run(newOrder(kSessionA, kTraderA, book, Side::Buy, 50, 100, 1));
    harness.run(newOrder(kSessionB, kTraderB, book, Side::Buy, 49, 100, 1));
    check(harness.loop.orders().size() == 2, "two orders rest");

    harness.run(command(CommandType::SessionClosed, kSessionA, kTraderA));
    check(harness.loop.orders().size() == 1,
          "cancel-on-disconnect pulls only that session's orders");
  }
}

/*
The determinism claim, which is what would make golden-file regression
testing nearly free later: order ids come from a plain counter and OrderTime
comes from the loop's sequence number, so the same script produces the same
events every time.

The one exception is book ids. `OrderBook::instance_count` is a process-wide
static, so the second Harness in this process gets the next id, not the same
one. That is a property of the engine, not of the gateway, and it is why the
comparison below skips those two fields rather than pretending they match.
*/
void testDeterminism() {
  auto script{[](Harness& harness) {
    const uint64_t book{bootstrap(harness)};
    Command subscribe{command(CommandType::SubscribeMd, kSessionA, kTraderA)};
    subscribe.book_id = book;
    harness.run(subscribe);
    harness.run(newOrder(kSessionA, kTraderA, book, Side::Buy, 50, 100, 1));
    harness.run(newOrder(kSessionB, kTraderB, book, Side::Sell, 50, 60, 1));
    Command amend{command(CommandType::Amend, kSessionA, kTraderA)};
    amend.client_order_id = 1;
    amend.price = 49;
    amend.quantity = 30;
    harness.run(amend);
  }};

  Harness first{};
  Harness second{};
  script(first);
  script(second);

  const auto& a{first.sink.events()};
  const auto& b{second.sink.events()};
  check(a.size() == b.size(), "the same script emits the same event count");
  if (a.size() != b.size()) return;

  std::size_t differing{0};
  for (std::size_t i{0}; i < a.size(); ++i) {
    Event lhs{a[i]};
    Event rhs{b[i]};
    lhs.book_id = 0;
    rhs.book_id = 0;
    if (lhs.type == EventType::BookEntry ||
        lhs.type == EventType::CreateBookAck) {
      lhs.payload.book.book_id = 0;
      rhs.payload.book.book_id = 0;
    }
    if (lhs.type == EventType::PositionUpdate) {
      lhs.payload.pos.book_id = 0;
      rhs.payload.pos.book_id = 0;
    }
    if (std::memcmp(&lhs, &rhs, sizeof(Event)) != 0) ++differing;
  }
  check(differing == 0,
        "two runs of one script are bit-identical apart from book ids");
}

// ===========================================================================
// Layer 3 — codec round-trip and frame-parser fuzzing
// ===========================================================================

namespace Wire = Exchange::Net::Binary;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

/*
Zeroes the Event fields the binary protocol deliberately does not carry, so
the round-trip comparison can be an exact one.

session_id and trader_id are the SERVER's routing state — a client already
knows its own, and putting them on every message would be 8 wasted bytes per
frame. LogonAck is the exception, because that is where the client learns
them, so it is excluded from the normalization.
*/
void normalizeForCompare(Event& event) {
  if (event.type != EventType::LogonAck) {
    event.session_id = 0;
    event.trader_id = 0;
  }
  // LogonAckBody, BookBody and PositionBody have no flags field: nothing
  // about those messages is per-batch or per-fill.
  switch (event.type) {
    case EventType::LogonAck:
    case EventType::Reject:
    case EventType::CreateBookAck:
    case EventType::BookEntry:
    case EventType::BookListEnd:
    case EventType::PositionUpdate:
      event.flags = 0;
      break;
    default:
      break;
  }
  // `side` is only meaningful where a body carries it.
  if (event.type == EventType::LogonAck || event.type == EventType::Reject ||
      event.type == EventType::CreateBookAck ||
      event.type == EventType::BookEntry ||
      event.type == EventType::BookListEnd ||
      event.type == EventType::PositionUpdate) {
    event.side = Side::Buy;
  }
  // Neither a logon nor a reject is about a book: LogonAck concerns the
  // session, and Reject echoes back the ids the client sent. The book
  // messages carry their id inside the payload instead of the envelope.
  if (event.type == EventType::LogonAck || event.type == EventType::Reject) {
    event.book_id = 0;
  }
  event.reserved = 0;
}

bool roundTripsEqual(const Event& original) {
  std::vector<std::byte> frame{};
  if (!Wire::encode(original, 1, frame)) return false;

  Wire::Header header{};
  if (!Wire::readHeader(frame, header)) return false;
  const auto decoded{Wire::decodeEvent(
      header.type,
      std::span<const std::byte>{frame}.subspan(Wire::kHeaderSize))};
  if (!decoded.has_value()) return false;

  Event lhs{original};
  Event rhs{*decoded};
  normalizeForCompare(lhs);
  normalizeForCompare(rhs);
  return std::memcmp(&lhs, &rhs, sizeof(Event)) == 0;
}

void testBinaryCodecRoundTrip() {
  // --- client -> server: encode, decode, compare the resulting Command ---
  {
    std::vector<std::byte> frame{};
    const Wire::NewOrderBody body{.client_order_id = 42,
                                  .book_id = 7,
                                  .price = 1234,
                                  .quantity = 500,
                                  .side = 1,
                                  .tif = 1,
                                  .flags = 1,
                                  .pad = {}};
    Wire::encodeNewOrder(body, 9, frame);
    const auto result{Wire::decode(frame, kSessionA, kTraderA, 777)};
    check(result.status == Wire::DecodeStatus::Ok, "a new order decodes");
    check(result.consumed == frame.size(), "and consumes exactly its frame");
    const Command& command{result.command};
    check(command.type == CommandType::NewOrder &&
              command.client_order_id == 42 && command.book_id == 7 &&
              command.price == 1234 && command.quantity == 500 &&
              command.side == Side::Sell && command.tif == TimeInForce::Ioc &&
              (command.flags & CommandFlags::kMarket) != 0,
          "every new-order field survives the round trip");
    // Identity and receive time come from the session, never from the wire.
    check(command.session_id == kSessionA && command.trader_id == kTraderA &&
              command.recv_ts_ns == 777,
          "a client cannot assert its own identity or timestamp");
  }

  {
    std::vector<std::byte> frame{};
    Wire::encodeCreateBook(
        Wire::CreateBookBody{.symbol = {'N', 'V', 'D', 'A', 0, 0, 0, 0},
                             .price_scale = 4,
                             .pad = {}},
        1, frame);
    const auto result{Wire::decode(frame, kSessionA, kTraderA, 0)};
    check(result.status == Wire::DecodeStatus::Ok &&
              result.command.symbol.view() == "NVDA" && result.command.aux == 4,
          "a create-book round-trips its symbol and price scale");
  }

  {
    // A full 8-character ticker has no terminator, which is the case
    // Symbol::view() scans for and the one a naive strlen would overrun.
    std::vector<std::byte> frame{};
    Wire::encodeCreateBook(
        Wire::CreateBookBody{.symbol = {'1', '2', '3', '4', '5', '6', '7', '8'},
                             .price_scale = 2,
                             .pad = {}},
        1, frame);
    const auto result{Wire::decode(frame, kSessionA, kTraderA, 0)};
    check(result.status == Wire::DecodeStatus::Ok &&
              result.command.symbol.view() == "12345678",
          "an unterminated 8-character ticker decodes correctly");
  }

  // --- multiple frames in one buffer, the normal case for a stream ---
  {
    std::vector<std::byte> stream{};
    Wire::encodeSimple(MsgType::ListBooks, 1, stream);
    Wire::encodeCancel(Wire::CancelBody{.client_order_id = 5, .order_id = 0}, 2,
                       stream);
    std::size_t offset{0};
    std::size_t frames{0};
    while (offset < stream.size()) {
      const auto result{
          Wire::decode(std::span<const std::byte>{stream}.subspan(offset),
                       kSessionA, kTraderA, 0)};
      if (result.status != Wire::DecodeStatus::Ok) break;
      offset += result.consumed;
      ++frames;
    }
    check(frames == 2 && offset == stream.size(),
          "two frames in one buffer both parse");
  }

  // --- a truncated frame asks for more rather than guessing ---
  {
    std::vector<std::byte> frame{};
    Wire::encodeCancel(Wire::CancelBody{.client_order_id = 1, .order_id = 2}, 1,
                       frame);
    for (std::size_t prefix{0}; prefix < frame.size(); ++prefix) {
      const auto result{Wire::decode(
          std::span<const std::byte>{frame}.subspan(0, prefix), 1, 1, 0)};
      check(result.status == Wire::DecodeStatus::NeedMore,
            "a partial frame is never decoded");
    }
  }

  // --- server -> client: every event type survives encode/decode ---
  auto event{[](EventType type) {
    Event e{};
    e.type = type;
    e.session_id = 0x0100'0009;
    e.trader_id = 3;
    e.book_id = 11;
    e.side = Side::Sell;
    e.flags = EventFlags::kEndOfBatch | EventFlags::kAggressor;
    return e;
  }};

  Event logon{event(EventType::LogonAck)};
  logon.payload.ack = AckPayload{.order_id = 0,
                                 .client_order_id = 0,
                                 .orig_order_id = 0,
                                 .price = 0,
                                 .quantity = 4,
                                 .reject_code = RejectCode::None,
                                 .pad = {}};
  check(roundTripsEqual(logon), "LogonAck round-trips");

  Event reject{event(EventType::Reject)};
  reject.payload.ack = AckPayload{.order_id = 8,
                                  .client_order_id = 9,
                                  .orig_order_id = 0,
                                  .price = 0,
                                  .quantity = 0,
                                  .reject_code = RejectCode::Throttled,
                                  .pad = {}};
  check(roundTripsEqual(reject), "Reject round-trips");

  for (const EventType type :
       {EventType::OrderAck, EventType::CancelAck, EventType::AmendAck}) {
    Event ack{event(type)};
    ack.payload.ack = AckPayload{.order_id = 100,
                                 .client_order_id = 101,
                                 .orig_order_id = 99,
                                 .price = 5000,
                                 .quantity = 25,
                                 .reject_code = RejectCode::None,
                                 .pad = {}};
    check(roundTripsEqual(ack), "an ack round-trips");
  }

  Event exec{event(EventType::ExecReport)};
  exec.payload.exec = ExecPayload{.exec_id = 7,
                                  .order_id = 8,
                                  .client_order_id = 9,
                                  .price = 5000,
                                  .quantity = 10,
                                  .leaves = 90};
  check(roundTripsEqual(exec), "ExecReport round-trips");

  for (const EventType type : {EventType::SnapshotBegin, EventType::LevelUpdate,
                               EventType::SnapshotEnd, EventType::TradePrint}) {
    Event md{event(type)};
    md.payload.md = MdPayload{.md_seq = 12,
                              .price = 5000,
                              .quantity = 250,
                              .ts_ns = 1'700'000'000,
                              .aggregate = 6,
                              .depth = 10,
                              .level_side = Side::Sell,
                              .pad = {}};
    check(roundTripsEqual(md), "a market-data message round-trips");
  }

  for (const EventType type : {EventType::CreateBookAck, EventType::BookEntry,
                               EventType::BookListEnd}) {
    Event book{event(type)};
    book.payload.book = BookPayload{.symbol = Symbol{"NVDA"},
                                    .book_id = 11,
                                    .price_scale = 2,
                                    .index = 1,
                                    .count = 3,
                                    .reject_code = RejectCode::None,
                                    .pad = {}};
    check(roundTripsEqual(book), "a book message round-trips");
  }

  Event position{event(EventType::PositionUpdate)};
  position.payload.pos = PosPayload{.book_id = 11,
                                    .net_quantity = -50,
                                    .avg_cost = 5000,
                                    .realized_pnl = -1234,
                                    .unrealized_pnl = 99,
                                    .mark_price = 5010};
  check(roundTripsEqual(position), "PositionUpdate round-trips");

  // Every named message type must actually be nameable in both directions —
  // this is what stops the two codecs drifting apart.
  for (const MessageName& entry : kMessageNames) {
    check(typeOf(entry.name) == entry.type, "the name table is a bijection");
  }
}

/*
Random bytes into the frame parser.

The parser is the only code in the server that runs before authentication, on
input an attacker fully controls. It has one job under garbage: never read
out of bounds, and either consume a frame or ask for more — never both, and
never neither. Under ASan and UBSan, "did not crash" is a real assertion.
*/
void testFrameParserFuzz() {
  std::mt19937_64 rng{0xF122ED};  // fixed seed: a failure must be repeatable
  std::vector<std::byte> buffer{};
  std::size_t decoded{0};
  std::size_t rejected{0};

  for (int iteration{0}; iteration < 20'000; ++iteration) {
    const std::size_t size{static_cast<std::size_t>(rng() % 96)};
    buffer.resize(size);
    for (std::byte& byte : buffer) {
      byte = static_cast<std::byte>(rng() & 0xFFu);
    }
    // Half the time, force a plausible header so the parser gets past the
    // cheap length check and into the body handlers.
    if (size >= Wire::kHeaderSize && (rng() & 1u) != 0) {
      const Wire::Header header{.length = static_cast<uint16_t>(size),
                                .type = static_cast<MsgType>(rng() & 0x7Fu),
                                .version = Wire::kProtocolVersion,
                                .seq = static_cast<uint32_t>(rng())};
      std::memcpy(buffer.data(), &header, Wire::kHeaderSize);
    }

    const auto result{Wire::decode(buffer, 1, 1, 0)};
    check(result.consumed <= buffer.size(),
          "the parser never claims to consume more than it was given");
    switch (result.status) {
      case Wire::DecodeStatus::Ok:
        check(result.consumed > 0, "a decoded frame consumes something");
        ++decoded;
        break;
      case Wire::DecodeStatus::UnknownType:
        check(result.consumed > 0, "a skipped frame consumes something");
        ++rejected;
        break;
      case Wire::DecodeStatus::NeedMore:
      case Wire::DecodeStatus::Malformed:
        break;
    }
  }
  // Not an assertion about correctness so much as about the test itself: if
  // the fuzzer never reached a body handler, it was testing nothing.
  check(decoded + rejected > 100, "the fuzzer actually reached the handlers");
}

// ===========================================================================
// Layer 4 — in-process loopback, end to end over a real socket
// ===========================================================================

// A minimal blocking client. Not exchange_cli: this one asserts rather than
// prints, and it must not depend on the tool building.
class LoopbackClient {
 public:
  explicit LoopbackClient(asio::io_context& io) : m_socket(io) {}

  bool connect(uint16_t port) {
    boost::system::error_code ec{};
    m_socket.connect(tcp::endpoint{asio::ip::make_address("127.0.0.1"), port},
                     ec);
    if (ec) return false;
    m_socket.set_option(tcp::no_delay{true}, ec);
    return true;
  }

  void send(const std::vector<std::byte>& frame) {
    boost::system::error_code ec{};
    asio::write(m_socket, asio::buffer(frame), ec);
  }

  // Reads until `wanted` arrives or the deadline passes. Returns the event.
  std::optional<Event> await(MsgType wanted, std::chrono::milliseconds timeout =
                                                 std::chrono::seconds{2}) {
    const auto deadline{std::chrono::steady_clock::now() + timeout};
    for (;;) {
      // Anything already buffered first.
      while (m_size >= Wire::kHeaderSize) {
        Wire::Header header{};
        std::memcpy(&header, m_buffer.data(), Wire::kHeaderSize);
        if (header.length < Wire::kHeaderSize || m_size < header.length) break;
        const auto decoded{Wire::decodeEvent(
            header.type,
            std::span<const std::byte>{m_buffer.data() + Wire::kHeaderSize,
                                       header.length - Wire::kHeaderSize})};
        const MsgType type{header.type};
        std::memmove(m_buffer.data(), m_buffer.data() + header.length,
                     m_size - header.length);
        m_size -= header.length;
        m_received.push_back(type);
        if (type == wanted) return decoded;
      }

      if (std::chrono::steady_clock::now() > deadline) return std::nullopt;

      boost::system::error_code ec{};
      const std::size_t bytes{m_socket.read_some(
          asio::buffer(m_buffer.data() + m_size, m_buffer.size() - m_size),
          ec)};
      if (ec == asio::error::would_block || ec == asio::error::try_again) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        continue;
      }
      if (ec) return std::nullopt;
      m_size += bytes;
    }
  }

  std::size_t countReceived(MsgType type) const {
    return static_cast<std::size_t>(std::ranges::count(m_received, type));
  }

  tcp::socket& socket() { return m_socket; }

 private:
  tcp::socket m_socket;
  std::vector<std::byte> m_buffer = std::vector<std::byte>(64 * 1024);
  std::size_t m_size{0};
  std::vector<MsgType> m_received{};
};

void testLoopback() {
  ServerConfig config{};
  config.binary_port = 0;  // let the OS choose, so the test never collides
  config.io_threads = 1;
  config.traders_path = "config/traders.example.json";

  Server server{config};
  std::string error{};
  if (!server.start(error)) {
    check(false, "the loopback server starts");
    std::println("  (start failed: {})", error);
    return;
  }
  const uint16_t port{server.binaryPort()};

  asio::io_context io{1};
  LoopbackClient alice{io};
  LoopbackClient bob{io};
  check(alice.connect(port) && bob.connect(port),
        "two clients connect over a real socket");
  alice.socket().non_blocking(true);
  bob.socket().non_blocking(true);

  std::vector<std::byte> frame{};
  uint32_t seq{0};

  Wire::encodeLogon("alice-dev-key-change-me", false, ++seq, frame);
  alice.send(frame);
  const auto alice_logon{alice.await(MsgType::LogonAck)};
  check(alice_logon.has_value() &&
            alice_logon->payload.ack.reject_code == RejectCode::None,
        "a valid api key logs on");

  frame.clear();
  Wire::encodeLogon("bob-dev-key-change-me", false, ++seq, frame);
  bob.send(frame);
  check(bob.await(MsgType::LogonAck).has_value(), "so does the second one");

  // A wrong key must be refused, and the connection dropped.
  {
    LoopbackClient impostor{io};
    check(impostor.connect(port), "an unauthenticated client can connect");
    impostor.socket().non_blocking(true);
    std::vector<std::byte> bad{};
    Wire::encodeLogon("definitely-not-a-real-key", false, 1, bad);
    impostor.send(bad);
    const auto refusal{impostor.await(MsgType::Reject)};
    check(refusal.has_value() &&
              refusal->payload.ack.reject_code == RejectCode::AuthFailed,
          "a bad api key is refused before anything else happens");
  }

  frame.clear();
  Wire::encodeCreateBook(
      Wire::CreateBookBody{.symbol = {'L', 'O', 'O', 'P', 0, 0, 0, 0},
                           .price_scale = 2,
                           .pad = {}},
      ++seq, frame);
  alice.send(frame);
  const auto created{alice.await(MsgType::CreateBookAck)};
  check(created.has_value() &&
            created->payload.book.reject_code == RejectCode::None,
        "a book is created over the wire");
  const uint64_t book{created.has_value() ? created->payload.book.book_id : 0};

  // Alice rests an offer.
  frame.clear();
  Wire::encodeNewOrder(Wire::NewOrderBody{.client_order_id = 1,
                                          .book_id = book,
                                          .price = 50,
                                          .quantity = 100,
                                          .side = 1,
                                          .tif = 0,
                                          .flags = 0,
                                          .pad = {}},
                       ++seq, frame);
  alice.send(frame);
  const auto rested{alice.await(MsgType::OrderAck)};
  check(rested.has_value() && rested->payload.ack.quantity == 100,
        "the offer rests in full");

  // Bob lifts part of it. Both sides must see their own fill.
  frame.clear();
  Wire::encodeNewOrder(Wire::NewOrderBody{.client_order_id = 1,
                                          .book_id = book,
                                          .price = 50,
                                          .quantity = 60,
                                          .side = 0,
                                          .tif = 0,
                                          .flags = 0,
                                          .pad = {}},
                       ++seq, frame);
  bob.send(frame);

  const auto bob_fill{bob.await(MsgType::ExecReport)};
  check(bob_fill.has_value() && bob_fill->payload.exec.quantity == 60 &&
            bob_fill->payload.exec.price == 50 &&
            bob_fill->payload.exec.leaves == 0,
        "the taker is filled in full at the resting price");
  check(bob_fill.has_value() && (bob_fill->flags & EventFlags::kAggressor) != 0,
        "and is marked as the aggressor");

  const auto alice_fill{alice.await(MsgType::ExecReport)};
  check(alice_fill.has_value() && alice_fill->payload.exec.quantity == 60 &&
            alice_fill->payload.exec.leaves == 40,
        "the maker sees the same fill, with 40 still working");
  check(alice_fill.has_value() &&
            (alice_fill->flags & EventFlags::kAggressor) == 0,
        "and is not marked as the aggressor");

  // Both counterparties get a position, from opposite sides.
  const auto bob_position{bob.await(MsgType::PositionUpdate)};
  const auto alice_position{alice.await(MsgType::PositionUpdate)};
  check(
      bob_position.has_value() && bob_position->payload.pos.net_quantity == 60,
      "the buyer is long 60");
  check(alice_position.has_value() &&
            alice_position->payload.pos.net_quantity == -60,
        "the seller is short 60");

  // Cancelling the remainder returns exactly what was left.
  frame.clear();
  Wire::encodeCancel(
      Wire::CancelBody{
          .client_order_id = 0,
          .order_id = rested.has_value() ? rested->payload.ack.order_id : 0},
      ++seq, frame);
  alice.send(frame);
  const auto cancelled{alice.await(MsgType::CancelAck)};
  check(cancelled.has_value() && cancelled->payload.ack.quantity == 40,
        "cancelling a partially filled order pulls the remaining 40");

  server.stop();
}

void testRejectCodes() {
  using Exchange::Types::EngineError;

  check(toRejectCode(EngineError::OrderNotFound) == RejectCode::UnknownOrder,
        "OrderNotFound maps to UnknownOrder");
  check(toRejectCode(EngineError::OrderBookNotFound) == RejectCode::UnknownBook,
        "OrderBookNotFound maps to UnknownBook");
  check(toWire(RejectCode::NotYourOrder) == RejectCode::UnknownOrder,
        "NotYourOrder is indistinguishable from UnknownOrder on the wire");
  check(toString(RejectCode::Throttled) == "Throttled",
        "reject codes stringify for logging");
}
}  // namespace

int main() {
  testRejectCodes();

  testRingBasics();
  testRingBatch();
  testRingThreaded();

  testLogonAndBookAdmin();
  testOrderRestsAndValidation();
  testCrossingProducesTwoExecReports();
  testCancelSemantics();
  testAmendMintsANewId();
  testIocAndMarketOrders();
  testMarketData();
  testPositions();
  testDisconnect();
  testDeterminism();

  testBinaryCodecRoundTrip();
  testFrameParserFuzz();
  testLoopback();

  if (g_failures == 0) {
    std::println("net_smoke: all checks passed");
    return 0;
  }
  std::println("net_smoke: {} failure(s)", g_failures);
  return 1;
}
