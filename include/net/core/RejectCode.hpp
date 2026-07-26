#pragma once

#include <cstdint>
#include <string_view>

#include "types/EngineError.hpp"

namespace Exchange::Net {
using Exchange::Types::EngineError;

/*
Why the gateway owns a reject vocabulary rather than putting `EngineError` on
the wire: most rejections never reach the engine at all (auth, throttling,
malformed frames, duplicate client order ids), and the two that do are
ambiguous from the engine's side. See `toRejectCode` and `toWire`.
*/
enum class RejectCode : uint8_t {
  None = 0,

  // Session / transport
  NotLoggedOn,
  AlreadyLoggedOn,
  AuthFailed,
  Throttled,
  MalformedMessage,
  UnsupportedVersion,

  // Order entry
  UnknownBook,
  UnknownSymbol,
  DuplicateSymbol,
  InvalidSymbol,
  InvalidQuantity,
  InvalidPrice,
  DuplicateClientOrderId,
  UnknownOrder,

  /*
  Server-side only. This must never reach a client: answering "that order
  exists but is not yours" turns the protocol into an order-id enumeration
  oracle. `toWire` collapses it to `UnknownOrder`; the distinct code exists so
  the server log can tell the two apart.
  */
  NotYourOrder,

  InternalError,
};

/*
`EngineError::OrderNotFound` is overloaded — it means both "no such order" and
"that order was already fully filled, so there is nothing left to cancel". The
engine cannot distinguish them (a fully-filled aggressor is never indexed).
The gateway can, from its own OrderStore, and does so before ever calling into
the engine; by the time this mapping runs, `UnknownOrder` is the honest answer.
*/
constexpr RejectCode toRejectCode(EngineError error) noexcept {
  switch (error) {
    case EngineError::Success:
      return RejectCode::None;
    case EngineError::OrderNotFound:
      return RejectCode::UnknownOrder;
    case EngineError::OrderBookNotFound:
      return RejectCode::UnknownBook;
    case EngineError::SymbolNotFound:
      return RejectCode::UnknownSymbol;
    case EngineError::DuplicateSymbol:
      return RejectCode::DuplicateSymbol;
    case EngineError::SymbolTooLong:
      return RejectCode::InvalidSymbol;
  }
  return RejectCode::InternalError;
}

// The only value that differs between the log and the wire.
constexpr RejectCode toWire(RejectCode code) noexcept {
  return code == RejectCode::NotYourOrder ? RejectCode::UnknownOrder : code;
}

std::string_view toString(RejectCode) noexcept;

static_assert(toRejectCode(EngineError::SymbolTooLong) ==
              RejectCode::InvalidSymbol);
static_assert(toWire(RejectCode::NotYourOrder) == RejectCode::UnknownOrder);
}  // namespace Exchange::Net
