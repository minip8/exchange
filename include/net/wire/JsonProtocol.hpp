#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "net/core/Command.hpp"
#include "net/core/Event.hpp"
#include "net/wire/MessageNames.hpp"

namespace Exchange::Net::Json {
/*
The browser-facing codec: the same Command and Event, over WebSocket text
frames.

Two rules keep this from drifting away from the binary protocol, which is the
real risk of shipping two codecs:

  1. The "type" field is the string from MessageNames.hpp — the same table
     the binary codec names its types with. There is no second list.
  2. Every JSON field name is byte-identical to the corresponding binary
     struct field. `client_order_id` is `client_order_id` in both. A reader
     of one protocol can read the other.

net_smoke round-trips every message through both codecs and asserts the
resulting Command/Event compare equal, which is what actually enforces this.

Prices are INTEGERS in both protocols. The GUI divides by 10^price_scale for
display. No floats ever go on the wire — two clients disagreeing about their
own money by a rounding step is not a bug worth having.
*/

enum class JsonStatus : uint8_t {
  Ok,
  Malformed,    // not JSON, or missing/!string "type"
  UnknownType,  // valid JSON naming a message that does not exist
};

struct JsonResult {
  JsonStatus status{JsonStatus::Malformed};
  Command command{};
  MsgType type{MsgType::None};
  // Logon only. Kept out of the Command deliberately: authentication is an
  // I/O-thread concern and secrets never cross the ring.
  std::string api_key{};
};

JsonResult decode(std::string_view text, uint32_t session_id,
                  uint32_t trader_id, uint64_t recv_ts_ns);

// Returns an empty string for events with no wire representation.
std::string encode(const Event& event);

// Only for the codec-equivalence test in net_smoke; nothing in the server
// parses its own output.
std::optional<Event> decodeEvent(std::string_view text);
}  // namespace Exchange::Net::Json
