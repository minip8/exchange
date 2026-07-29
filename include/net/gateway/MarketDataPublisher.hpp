#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "engine/OrderBook.hpp"
#include "net/core/Event.hpp"
#include "net/core/Ids.hpp"
#include "net/core/Side.hpp"
#include "net/core/Tuning.hpp"
#include "types/OrderBookId.hpp"

namespace Exchange::Net {
using Exchange::Engine::OrderBook;
using Exchange::Types::OrderBookId;

struct MdLevel {
  uint64_t price{};
  uint64_t quantity{};
  bool operator==(const MdLevel&) const noexcept = default;
};

/*
L2 depth deltas plus a trade tape.

Top-of-book alone cannot reconstruct a ladder, and full depth is pointless at
this scale, so the feed is a fixed window of the best `depth` levels per side
(default 10). A LevelUpdate with quantity 0 means the level left the client's
view — the standard L2 convention, which is why there is no delete message.

Depth is a property of the BOOK, not of the subscriber. That is deliberate:
md_seq is per book, so if two subscribers at different depths received
different message streams, one of them would see gaps in a sequence that has
none. SubscribeMd's requested depth is therefore advisory, and the book's
actual depth is reported back in SnapshotBegin.

--- On sequencing, which is the elegant part of the whole design ---

SubscribeMd travels through the same ingress ring as every other command, so
the matching thread handles it *between* two commands, when the book is
quiescent, and emits the entire snapshot in one tryPushBatch. Because there
is exactly one reader of book state doing exactly one thing at a time, the
ring's total order gives snapshot/delta consistency for free: a late
subscriber can neither receive a delta with md_seq <= its snapshot's, nor
miss one above it. There is no gap window and no race to close, and none of
the usual "buffer deltas while the snapshot is built" machinery is needed.
This is the single best argument for the single-writer design.

--- On how deltas are computed ---

The obvious formulation is to track dirty prices (fills enumerate every
touched opposite level, since Fill::price is the resting price; a rest or a
cancel contributes one more) and look each one up. That is O(dirty x log L),
but it does not account for the depth boundary: when a level is deleted, the
level at rank `depth` becomes visible and must be sent even though nothing
about it changed, and symmetrically a new best level pushes one out of view.

So instead the publisher keeps the window it last published and diffs the
current window against it. Reading the window is O(depth x k) — the level
vectors are sorted worst-first with empty levels erased, so the top N is just
the last N entries read backwards — which at depth 10 and k of 1-3 is the same
order of cost, and it is the only formulation that is actually correct at the
boundary. The dirty-price idea survives as the gate: books with no
subscribers, and commands that changed nothing, skip the work entirely.

The diff itself is a two-pointer merge, not a pair of nested searches. Both
windows are sorted best-first for their side (readWindow reverses), so walking
them together is O(depth) where looking each level up in the other window was
O(depth^2) — and this runs on every order, cancel and amend that has a
subscriber, so the constant is not academic.
*/
class MarketDataPublisher {
 public:
  void registerBook(OrderBookId book_id, uint32_t depth);

  // Returns false if the book is unknown or the session already subscribes.
  bool subscribe(OrderBookId book_id, SessionId session_id);
  bool unsubscribe(OrderBookId book_id, SessionId session_id);
  void dropSession(SessionId session_id);

  bool hasSubscribers(OrderBookId book_id) const;

  // SnapshotBegin + N x LevelUpdate + SnapshotEnd, appended to `out`. The
  // caller publishes the whole run as one batch.
  void emitSnapshot(const OrderBook& book, OrderBookId book_id,
                    SessionId session_id, TraderId trader_id, uint64_t ts_ns,
                    std::vector<Event>& out);

  // Diffs the current window against the last published one and appends a
  // LevelUpdate per changed level, to every subscriber.
  void publishDeltas(const OrderBook& book, OrderBookId book_id, uint64_t ts_ns,
                     std::vector<Event>& out);

  // Trade prints deliberately carry no order ids — that would leak
  // counterparty identity to the whole feed. The private ExecReport carries
  // them, to the two owners only.
  void publishTrade(OrderBookId book_id, uint64_t price, uint64_t quantity,
                    Side aggressor_side, uint64_t ts_ns,
                    std::vector<Event>& out);

  // Mid price for marking, in price units. Falls back to the one live side,
  // then to the last trade. nullopt if the book has never traded and has no
  // resting orders.
  std::optional<int64_t> mark(const OrderBook& book, OrderBookId book_id) const;

  uint64_t sequence(OrderBookId book_id) const;

 private:
  struct BookMd {
    uint64_t md_seq{0};
    uint32_t depth{kDefaultMdDepth};
    uint64_t last_trade_price{0};
    bool has_traded{false};
    std::vector<MdLevel> published_buys{};
    std::vector<MdLevel> published_sells{};
    std::vector<SessionId> subscribers{};
  };

  static void readWindow(std::span<const Exchange::Engine::PriceLevel> levels,
                         uint32_t depth, std::vector<MdLevel>& out);

  void emitLevel(BookMd& md, OrderBookId book_id, Side side, uint64_t price,
                 uint64_t quantity, uint64_t ts_ns, std::vector<Event>& out);

  // Appends the LevelUpdates that turn `published` into `current`, then
  // adopts `current` as the new baseline.
  void diffSide(BookMd& md, OrderBookId book_id, Side side,
                const std::vector<MdLevel>& current,
                std::vector<MdLevel>& published, uint64_t ts_ns,
                std::vector<Event>& out);

  std::unordered_map<OrderBookId, BookMd> m_books{};
  // Scratch buffers, reused across calls so the steady state never allocates.
  mutable std::vector<MdLevel> m_scratch_buys{};
  mutable std::vector<MdLevel> m_scratch_sells{};
};
}  // namespace Exchange::Net
