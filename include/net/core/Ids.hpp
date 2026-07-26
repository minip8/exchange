#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace Exchange::Net {
/*
The gateway's two 32-bit identity types.

Both were bare uint32_t, and they appear next to each other in a great many
signatures — emitSnapshot(book, book_id, session_id, trader_id, ...) being the
worst of them. Swapping the pair compiled silently and produced events
addressed to a session that does not exist, which is about the least
debuggable failure this layer can produce. Making them distinct types is the
whole of the fix.

They follow the include/types/ strong-typedef pattern with one deviation: a
defaulted default constructor. Types::OrderId has none, which is fine for a
type that is always built from a value, but these are members of
value-initialized structs (`Event event{}`, OrderMeta's NSDMIs) and so must be
default-constructible.

--- Where these are used, and where they deliberately are not ---

They are used on the GATEWAY side — OrderStore, Positions,
MarketDataPublisher, MatchingLoop — because that is where the hazard actually
lives: those classes have typed function signatures taking both ids, often
adjacent, and the compiler can now reject a swap.

They are NOT used in Command and Event, which keep raw uint32_t. Those
structs are exactly one cache line, trivially copyable, and read by two
codecs that do nothing but memcpy. A wrapper buys no safety there — a codec
that confuses two adjacent uint32_t reads is confusing byte offsets, which no
type can catch — and costs a .value at every encode and decode site.
Conversion happens at the gateway boundary, exactly as Side does.

They are NOT used in net_io either, and that is worth justifying rather than
leaving as an inconsistency. Every id-taking signature there — SessionTable,
EgressQueue::requestDisconnect, IoThread::dispatch, ClientSession::sessionId
— takes a session id and nothing else, so there is no second uint32_t for it
to be confused with and nothing for a distinct type to reject. The one place
in net_io holding both ids is TcpSession/WebSocketSession's member pair, and
typing those would still not help: the risky line is

    command.session_id = m_session_id;   // or, swapped, m_trader_id

which assigns into Command's raw uint32_t and compiles either way, with or
without a .value. Wrapping there would add noise at the densest raw-wire
boundary in the codebase and catch nothing.
*/
struct SessionId {
  using T = uint32_t;
  T value{};

  SessionId() = default;
  explicit SessionId(T v) : value(v) {}

  std::strong_ordering operator<=>(const SessionId&) const = default;
  bool operator==(const SessionId&) const = default;
};

struct TraderId {
  using T = uint32_t;
  T value{};

  TraderId() = default;
  explicit TraderId(T v) : value(v) {}

  std::strong_ordering operator<=>(const TraderId&) const = default;
  bool operator==(const TraderId&) const = default;
};

// 0 is never handed out as a session id — it is the "no session" sentinel
// used throughout the gateway, and IoThread::nextSessionId starts its
// per-thread counter at 1 to keep it free.
inline constexpr SessionId kNoSession{};

static_assert(sizeof(SessionId) == sizeof(uint32_t));
static_assert(sizeof(TraderId) == sizeof(uint32_t));
}  // namespace Exchange::Net

template <>
struct std::hash<Exchange::Net::SessionId> {
  size_t operator()(const Exchange::Net::SessionId& id) const noexcept {
    return std::hash<uint32_t>{}(id.value);
  }
};

template <>
struct std::hash<Exchange::Net::TraderId> {
  size_t operator()(const Exchange::Net::TraderId& id) const noexcept {
    return std::hash<uint32_t>{}(id.value);
  }
};
