#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "net/core/Event.hpp"

namespace Exchange::Net {
/*
What an I/O thread needs from a session, regardless of how it talks.

The binary and WebSocket transports differ entirely below this line — framing
versus message boundaries, memcpy versus JSON, one coalesced write versus a
queue of frames — and not at all above it. Egress routing, the session table
and shutdown all work through this interface, so the matching thread genuinely
cannot tell which kind of client it is talking to.

One virtual call per delivered batch, not per event.
*/
class ClientSession {
 public:
  ClientSession() = default;
  ClientSession(const ClientSession&) = delete;
  ClientSession& operator=(const ClientSession&) = delete;
  virtual ~ClientSession() = default;

  // Always called on this session's own I/O thread.
  virtual void deliver(std::span<const Event> events) = 0;
  virtual void close(std::string_view reason) = 0;
  virtual uint32_t sessionId() const noexcept = 0;
};
}  // namespace Exchange::Net
