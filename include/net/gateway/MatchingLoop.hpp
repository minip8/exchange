#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "engine/MatchingEngine.hpp"
#include "net/core/Command.hpp"
#include "net/core/Event.hpp"
#include "net/core/RejectCode.hpp"
#include "net/gateway/BookDirectory.hpp"
#include "net/gateway/EventSink.hpp"
#include "net/gateway/MarketDataPublisher.hpp"
#include "net/gateway/OrderStore.hpp"
#include "net/gateway/Positions.hpp"

#ifndef NDEBUG
#include <thread>
#endif

namespace Exchange::Net {
using Exchange::Engine::MatchingEngine;

struct MatchingLoopConfig {
  uint32_t default_md_depth{10};
  uint32_t default_price_scale{2};
  // Whether an unauthenticated-but-connected session may create books. The
  // gateway has no roles yet; this is the whole of book-admin authorization.
  bool allow_client_book_creation{true};
};

/*
Everything that happens on the matching thread, and nothing that does not.

This class links `engine` and net_protocol only — no Boost, no sockets, no
threads — so every rule it enforces is exercisable synchronously from
net_smoke through a VectorEventSink. See EventSink.hpp.

--- Determinism ---

Order ids come from a plain non-atomic counter here, and OrderTime comes from
this loop's monotonic sequence number rather than a clock. Consequently a
scripted vector<Command> produces a bit-identical vector<Event> on every run,
which is what would make golden-file regression testing nearly free later.

--- Why there is exactly one id minter, and why it lives here ---

`Order::instance_count` and `OrderBook::instance_count` are non-atomic
`static inline` counters, so constructing those objects on two threads is a
data race. Minting ids here and using Order's explicit-id constructor
exclusively settles every hazard at once:

  - Duplicate order ids become structurally impossible, so
    OrderBook::tryInsertRestingOrder's `insert_or_assign` can never silently
    corrupt the index (id uniqueness is the gateway's job, and this is how it
    does it).
  - `Order::instance_count` is never written in this process, so the
    non-atomic static is not a race, and NO engine change is required.
  - 0 stays free as a wire sentinel for "no order id".
  - Ids stay dense, so OrderStore could later become a slab indexed by id-1.

`OrderBook` construction DOES bump its counter, which is why book creation
must happen inside the CreateBook handler, on this thread. A debug assert
backs that rule.

--- Three id spaces, never conflated ---

  client_order_id  client-owned, unique per session
  order_id         minted here, process-global, never reused
  exec_id          minted here, one per fill leg; the client's dedup key

--- Two clocks, never conflated ---

  Command::recv_ts_ns  wall clock, stamped on the I/O thread, for reports
  m_seq -> OrderTime   priority only; zero clock cost, and it satisfies the
                       book's non-decreasing-time invariant by construction
*/
class MatchingLoop {
 public:
  explicit MatchingLoop(EventSink& sink, MatchingLoopConfig config = {});

  MatchingLoop(const MatchingLoop&) = delete;
  MatchingLoop& operator=(const MatchingLoop&) = delete;

  // Handles one command and publishes everything it produced as one batch.
  void handle(const Command& command);

  // Handles a run of commands. Each still publishes its own batch: that
  // keeps kEndOfBatch meaningful per command, and bounds a batch to something
  // the egress ring can always accept whole.
  void handleBatch(std::span<const Command> commands);

  const BookDirectory& books() const noexcept { return m_directory; }
  const OrderStore& orders() const noexcept { return m_orders; }
  const Positions& positions() const noexcept { return m_positions; }
  const MatchingEngine& engine() const noexcept { return m_engine; }
  const MarketDataPublisher& marketData() const noexcept { return m_publisher; }
  uint64_t sequence() const noexcept { return m_seq; }

 private:
  struct SessionInfo {
    uint32_t trader_id{};
    bool cancel_on_disconnect{false};
  };

  // --- command handlers ---
  void onSessionOpened(const Command&);
  void onSessionClosed(const Command&);
  void onCreateBook(const Command&);
  void onListBooks(const Command&);
  void onNewOrder(const Command&);
  void onCancel(const Command&);
  void onAmend(const Command&);
  void onSubscribeMd(const Command&);
  void onUnsubscribeMd(const Command&);
  void onGetSnapshot(const Command&);

  // --- emission helpers ---
  Event base(const Command&, EventType) const;
  void reject(const Command&, RejectCode);
  // Private events every one of a trader's sessions should see (fills,
  // positions). Acks go only to the session that asked.
  void emitToTrader(uint32_t trader_id, const Event& prototype);
  // `origin_session` is the session that sent the command, or 0 for
  // commands with nobody left to answer (SessionClosed).
  void flush(uint32_t origin_session);

  // Applies one fill leg: two exec reports, a trade print, and both
  // counterparties' position updates.
  void applyFill(const Command&, const Exchange::Engine::Fill&,
                 OrderBookId book_id, uint32_t aggressor_trader,
                 uint64_t aggressor_coid, uint64_t aggressor_leaves);
  void emitPosition(uint32_t trader_id, OrderBookId book_id);
  void republish(OrderBookId book_id, uint64_t ts_ns);

  // Resolves a Cancel/Amend target, applying the ownership rule. On failure
  // it has already emitted the reject.
  bool resolveTarget(const Command&, OrderId& id, OrderMeta& meta);

  Exchange::Types::OrderTime nextTime();

  EventSink& m_sink;
  MatchingLoopConfig m_config;

  MatchingEngine m_engine{};
  BookDirectory m_directory{};
  OrderStore m_orders{};
  Positions m_positions{};
  MarketDataPublisher m_publisher{};

  std::unordered_map<uint32_t, SessionInfo> m_sessions{};
  std::unordered_map<uint32_t, std::vector<uint32_t>> m_trader_sessions{};

  // 0 is reserved as the wire sentinel for "no order id".
  uint64_t m_next_order_id{1};
  uint64_t m_next_exec_id{1};
  uint64_t m_seq{0};

  std::vector<Event> m_out{};

#ifndef NDEBUG
  std::thread::id m_owner_thread{};
#endif
};
}  // namespace Exchange::Net
