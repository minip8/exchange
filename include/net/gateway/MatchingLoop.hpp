#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "engine/MatchingEngine.hpp"
#include "net/core/Command.hpp"
#include "net/core/Event.hpp"
#include "net/core/Ids.hpp"
#include "net/core/RejectCode.hpp"
#include "net/core/Tuning.hpp"
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
  uint32_t default_md_depth{kDefaultMdDepth};
  uint32_t default_price_scale{kDefaultPriceScale};
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

--- Protocol-visible properties worth knowing before reading the handlers ---

Two things here are deliberate and observable from a client, and both would
look like bugs without this note:

  Amend mints a NEW order id, and priority is always lost — even when the
  amend only shrinks quantity. See the comment on onAmend.

  A command emits ONE PositionUpdate per (trader, book), not one per fill
  leg. An aggressor sweeping twenty resting orders gets a single position
  message carrying the end state, not twenty carrying each intermediate. The
  intermediates were never wrong, just redundant: addOrder completes the
  whole match before any leg is reported, so every one of them would have
  carried the same mark. ExecReports are unaffected — a client reconstructing
  its position from the exec stream, which is the normal design, sees no
  difference at all.
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
    TraderId trader_id{};
    bool cancel_on_disconnect{false};
  };

  // The outcome of resolving a Cancel/Amend target. Detached from the store,
  // because the caller is about to erase the entry it came from.
  struct ResolvedTarget {
    OrderId id{0};
    OrderMeta meta{};
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
  void emitToTrader(TraderId trader_id, const Event& prototype);
  // `origin_session` is the session that sent the command, or kNoSession for
  // commands with nobody left to answer (SessionClosed).
  void flush(SessionId origin_session);

  // Applies one fill leg: two exec reports and a trade print. Position
  // updates are NOT emitted here — they are accumulated in
  // m_touched_positions and flushed once per command by emitPositions().
  void applyFill(const Command&, const Exchange::Engine::Fill&,
                 OrderBookId book_id, TraderId aggressor_trader,
                 uint64_t aggressor_coid, uint64_t aggressor_leaves);
  // Records that this (trader, book) needs a PositionUpdate, deduplicating.
  void touchPosition(TraderId trader_id, OrderBookId book_id);
  // Emits one PositionUpdate per (trader, book) touched by this command, then
  // clears the set. See the protocol note in the class comment.
  void emitPositions();
  void republish(OrderBookId book_id, uint64_t ts_ns);

  // Resolves a Cancel/Amend target, applying the ownership rule. nullopt
  // means it has already emitted the reject.
  std::optional<ResolvedTarget> resolveTarget(const Command&);

  Exchange::Types::OrderTime nextTime();

  EventSink& m_sink;
  MatchingLoopConfig m_config;

  MatchingEngine m_engine{};
  BookDirectory m_directory{};
  OrderStore m_orders{};
  Positions m_positions{};
  MarketDataPublisher m_publisher{};

  std::unordered_map<SessionId, SessionInfo> m_sessions{};
  std::unordered_map<TraderId, std::vector<SessionId>> m_trader_sessions{};

  // 0 is reserved as the wire sentinel for "no order id".
  uint64_t m_next_order_id{1};
  uint64_t m_next_exec_id{1};
  uint64_t m_seq{0};

  std::vector<Event> m_out{};

  /*
  The (trader, book) pairs whose position changed while handling the current
  command, deduplicated so each gets exactly one PositionUpdate.

  A vector rather than a set: a command touches at most a handful of distinct
  traders even when it sweeps many levels, and a linear scan over four
  elements beats hashing every one of them. Cleared by emitPositions(), so it
  never grows beyond one command's worth.
  */
  struct TouchedPosition {
    TraderId trader_id{};
    OrderBookId book_id{0};
    bool operator==(const TouchedPosition&) const noexcept = default;
  };
  std::vector<TouchedPosition> m_touched_positions{};

#ifndef NDEBUG
  std::thread::id m_owner_thread{};
#endif
};
}  // namespace Exchange::Net
