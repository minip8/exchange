#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "net/core/CacheLine.hpp"
#include "net/core/RejectCode.hpp"
#include "net/core/Side.hpp"
#include "types/Symbol.hpp"

namespace Exchange::Net {
using Exchange::Types::Symbol;

enum class EventType : uint8_t {
  None = 0,

  // Session
  LogonAck,
  Reject,

  // Order entry
  OrderAck,
  ExecReport,
  CancelAck,
  AmendAck,

  // Book administration and discovery
  CreateBookAck,
  BookEntry,
  BookListEnd,

  // Market data
  SnapshotBegin,
  LevelUpdate,
  SnapshotEnd,
  TradePrint,
  // Confirms a subscribe or unsubscribe took effect. It exists partly so
  // that every client-originated command produces at least one event for its
  // own session, which is what keeps the in-flight count from leaking.
  MdAck,

  // Accounting
  PositionUpdate,
};

namespace EventFlags {
// Last event of a coherent group. The GUI coalesces rendering on this so a
// burst of level updates paints once, not once per level.
inline constexpr uint8_t kEndOfBatch{1u << 0};
// ExecReport: this side was the aggressor (took liquidity).
inline constexpr uint8_t kAggressor{1u << 1};
// ExecReport/CancelAck: the order is now fully done and has left the book.
inline constexpr uint8_t kFinal{1u << 2};
/*
The last event a command produced for the session that SENT it.

This is what makes the per-session in-flight count exact rather than
approximate. One command can produce many events, and some go to other
sessions entirely — a fill reports to both counterparties, a level update to
every subscriber — so "an event arrived" is not the same as "my command
finished". The matching thread knows which command it is handling, so it
marks the boundary rather than making the I/O thread infer it.
*/
inline constexpr uint8_t kCommandComplete{1u << 3};
}  // namespace EventFlags

/*
One leg of a fill, reported privately to one of the two owners.

--- What a "leg" is ---

An incoming order does not match "an order", it sweeps as much of the opposite
side as its price allows. Each resting order it consumes on the way is one
LEG. So a buy for 300 that crosses three resting sells of 100 produces three
legs, and every leg is reported to BOTH of its owners — the aggressor and that
particular resting counterparty — so the command emits six ExecReports with
six distinct exec_ids. There is no single "the fill" event, which is why the
client's dedup key is per leg rather than per order.

--- What "leaves" is ---

FIX's LeavesQty: how much of THIS owner's order is still open after THIS leg.
It is the field that makes the gateway, not the engine, the authority on order
state — the engine consumes an Order and hands back only fills, so nothing
else in the process knows how much of an order is left. See OrderStore.

The two reports of a single leg carry different leaves, which is the whole
reason it is per-owner. Continuing the example, with the aggressor's 300
against three resting 100s:

  leg 1   aggressor leaves 200   resting #1 leaves 0  (kFinal)
  leg 2   aggressor leaves 100   resting #2 leaves 0  (kFinal)
  leg 3   aggressor leaves 0     resting #3 leaves 0  (kFinal)
          (kFinal)

Had the third resting order been for 250, leg 3 would fill 100 of it and read
`aggressor leaves 0, resting #3 leaves 150` — the resting side stays open and
keeps its entry in the store.

`quantity` is that leg alone and never a running total; a client wanting
cumulative filled quantity computes original - leaves. `price` is always the
RESTING order's price, because that is the one that was on the book first and
therefore set the terms of the trade.
*/
struct ExecPayload {
  uint64_t exec_id;  // unique per leg; the client's dedup key
  uint64_t order_id;
  uint64_t client_order_id;
  uint64_t price;     // always the RESTING order's price
  uint64_t quantity;  // this leg only, not cumulative
  uint64_t leaves;    // remaining on THIS owner's order after this leg
};
static_assert(sizeof(ExecPayload) == 48);

// OrderAck / CancelAck / AmendAck / Reject / LogonAck.
struct AckPayload {
  uint64_t order_id;
  uint64_t client_order_id;
  // Amend only: the id being replaced. An amend always mints a NEW order id
  // (see MatchingLoop::onAmend), so the client needs the chain — this is the
  // same shape as FIX's OrigClOrdID.
  uint64_t orig_order_id;
  uint64_t price;
  uint64_t quantity;  // leaves at ack time; LogonAck: number of books
  RejectCode reject_code;
  uint8_t pad[7];
};
static_assert(sizeof(AckPayload) == 48);

struct MdPayload {
  // Per-book, monotonic, one per market-data message. Clients check
  // md_seq == last + 1 and resync with GetSnapshot on a gap.
  uint64_t md_seq;
  uint64_t price;
  // 0 on a LevelUpdate means the level is gone. That is the standard L2
  // convention and is why there is no separate delete message.
  uint64_t quantity;
  uint64_t ts_ns;      // recv_ts_ns of the command that caused this
  uint64_t aggregate;  // SnapshotBegin: total levels to follow
  uint32_t depth;      // SnapshotBegin: the book's published depth
  Side level_side;
  uint8_t pad[3];
};
static_assert(sizeof(MdPayload) == 48);

struct BookPayload {
  Symbol symbol;
  uint64_t book_id;
  uint32_t price_scale;  // wire prices are integers; divide by 10^price_scale
  uint32_t index;        // BookEntry: position in the listing
  uint32_t count;        // BookListEnd: total entries sent
  RejectCode reject_code;
  uint8_t pad[19];
};
static_assert(sizeof(BookPayload) == 48);

struct PosPayload {
  uint64_t book_id;
  int64_t net_quantity;    // signed: positive long, negative short
  int64_t avg_cost;        // weighted-average entry, in price units
  int64_t realized_pnl;    // in price x quantity units
  int64_t unrealized_pnl;  // marked against mark_price
  int64_t mark_price;
};
static_assert(sizeof(PosPayload) == 48);

/*
The egress message: a 16-byte envelope plus a 48-byte tagged union, one cache
line total, trivially copyable, no pointers.

Every Event is addressed to exactly one session — market-data fan-out happens
on the matching thread, which is what lets egress routing be the O(1)
`session_id >> 24` index into the I/O thread array.

A consequence worth stating up front, because it shapes the whole market-data
design: since Event is fixed-size, a snapshot is NOT one event. It is
SnapshotBegin + N x LevelUpdate + SnapshotEnd published in a single
tryPushBatch. That is how real feeds work, and it means there is no
variable-length cross-thread ownership contract to get wrong.

--- Why the payloads carry explicit pad[] members and not alignas ---

This looks like something alignas should express, and it is not. alignas
constrains where an object starts; it says nothing about TAIL padding, which
the compiler would still insert to round each 48-byte arm out. The difference
is what those bytes contain: padding the compiler inserts is INDETERMINATE,
while a `uint8_t pad[N]` member named in a designated initializer is zeroed
like any other field.

Three things here depend on that being zero rather than indeterminate:

  - net_smoke encodes the same content through the binary and JSON codecs and
    compares the resulting Command/Event with memcmp over the whole object.
    That is the anti-drift test between the two protocols, and indeterminate
    padding would make byte-identical messages compare unequal at random.
  - Event is memcpy'd into the egress ring and, for the binary protocol, its
    fields onto a socket. Indeterminate bytes are stack or heap residue, so
    this would be an information leak to clients as well as a bug.
  - The union means the arms not being written are dead storage; `payload{.raw
    = {}}` is what makes an Event constructed today byte-identical to the same
    Event constructed tomorrow, which is what the determinism claim in
    MatchingLoop.hpp rests on.

So the pads stay, and they stay explicit, because that is the only form that
is both reviewable and guaranteed zero.
*/
struct Event {
  uint32_t session_id{};
  uint32_t trader_id{};
  // Narrowed from OrderBookId's uint64: ids come from a dense counter, and
  // BookDirectory refuses to register one that does not fit.
  uint32_t book_id{};
  EventType type{EventType::None};
  uint8_t flags{};
  Side side{Side::Buy};
  uint8_t reserved{};

  union Payload {
    ExecPayload exec;
    AckPayload ack;
    MdPayload md;
    BookPayload book;
    PosPayload pos;
    uint64_t raw[6];
  } payload{.raw = {}};
};

static_assert(sizeof(Event) == 64, "Event must be exactly one cache line");
static_assert(alignof(Event) == 8);
static_assert(std::is_trivially_copyable_v<Event>);
static_assert(offsetof(Event, payload) == 16, "16-byte envelope");
static_assert(sizeof(Event) == kCacheLine,
              "the 64-byte layout assumes a 64-byte line");
}  // namespace Exchange::Net
