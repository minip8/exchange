#pragma once

#include <chrono>
#include <cstdint>
#include <type_traits>

#include "net/core/CacheLine.hpp"
#include "net/core/Side.hpp"
#include "types/Symbol.hpp"

namespace Exchange::Net {
using Exchange::Types::Symbol;

enum class CommandType : uint8_t {
  None = 0,

  // Session lifecycle. Neither may ever be dropped: a lost SessionClosed
  // leaks the session's routing and subscription entries forever.
  SessionOpened,
  SessionClosed,

  // Order entry.
  NewOrder,
  Cancel,
  Amend,

  // Book administration and discovery.
  CreateBook,
  ListBooks,

  // Market data.
  SubscribeMd,
  UnsubscribeMd,
  GetSnapshot,
};

enum class TimeInForce : uint8_t {
  Gtc = 0,
  Ioc,
};

namespace CommandFlags {
// Price is ignored; the gateway synthesizes a marketable limit. See
// MatchingLoop::onNewOrder.
inline constexpr uint8_t kMarket{1u << 0};
// SessionOpened only: pull this session's resting orders on disconnect.
inline constexpr uint8_t kCancelOnDisconnect{1u << 1};
}  // namespace CommandFlags

/*
The ingress message. Exactly one cache line, trivially copyable, and — this
is the load-bearing part — it contains no pointers, ever. A Command is copied
bit-for-bit into a ring slot; anything it referenced would be an ownership
contract across a thread boundary, which is precisely what this design exists
to avoid.

`recv_ts_ns` is stamped on the I/O thread and is the ONLY wall clock in the
system. It is used for reports and latency measurement and never for
priority; priority comes from the matching thread's monotonic sequence
number. Do not conflate them.

Order entry carries `book_id`, never `symbol`: the codebase's rule is that
symbol hashing stays off the matching hot path. Clients learn the
symbol -> book_id mapping from ListBooks at logon. The union is sound because
Symbol is trivially copyable and CommandType discriminates which arm is live
— only CreateBook reads `symbol`.
*/
struct Command {
  uint64_t recv_ts_ns{};       // wall clock, stamped on the I/O thread
  uint64_t client_order_id{};  // client-owned, unique per session
  uint64_t order_id{};         // target of Cancel/Amend; 0 on NewOrder
  uint64_t price{};
  uint64_t quantity{};
  union {
    uint64_t book_id;
    Symbol symbol;
  };
  uint32_t session_id{};
  uint32_t trader_id{};
  uint32_t aux{};  // SubscribeMd: requested depth. CreateBook: price scale.
  CommandType type{CommandType::None};
  Side side{Side::Buy};
  TimeInForce tif{TimeInForce::Gtc};
  uint8_t flags{};

  // The union member needs an explicit initializer; `book_id` covers both
  // arms since Symbol is 8 zero bytes when the id is 0.
  constexpr Command() noexcept : book_id{0} {}
};

static_assert(sizeof(Command) == 64, "Command must be exactly one cache line");
static_assert(alignof(Command) == 8);
static_assert(std::is_trivially_copyable_v<Command>);
static_assert(sizeof(Command) == kCacheLine,
              "the 64-byte layout assumes a 64-byte line");

/*
The one wall clock in the system, defined once beside the field it fills.

Both transports stamp `recv_ts_ns` with this as a client's bytes come off the
socket. It lived as an identical copy in TcpSession.cpp's and
WebSocketSession.cpp's anonymous namespaces, which left the "only wall clock"
rule above as a comment rather than as a fact: one of the two quietly changed
to steady_clock would have made the binary and WebSocket transports stamp
timestamps that cannot be compared with each other, and nothing would have
caught it. The field reaches clients as MdPayload::ts_ns and is the basis of
net_workload_bench's latency numbers, so that divergence would be wrong in two
places at once.

system_clock, not steady_clock, precisely because this IS the wall clock —
a browser rendering a trade tape wants a real timestamp. Priority never comes
from here; it comes from MatchingLoop's monotonic sequence number.
*/
inline uint64_t nowNs() noexcept {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}
}  // namespace Exchange::Net
