#pragma once

#include <span>
#include <vector>

#include "net/core/Event.hpp"

namespace Exchange::Net {
/*
The seam that keeps the matching thread testable.

MatchingLoop writes every one of its outputs through this interface, so it
links `engine` and net_protocol and nothing else — no Boost, no sockets, no
threads. Ownership checks, IOC synthesis, amend, `leaves` arithmetic, market
data sequencing and position maths are therefore all exercisable
synchronously from net_smoke. This is the single most important testability
decision in the networking layer; do not let I/O concerns leak across it.

`publish` takes a span rather than one event because delivery atomicity
matters: a snapshot must cross as a unit (see Event.hpp).

Backpressure policy lives in the implementation, not here, because the
correct answer differs per stream — a dropped exec report desynchronizes a
client's position and warrants a disconnect, while a dropped level update is
recoverable by resnapshot. See ThreadedEventSink.

--- Why this stays virtual, rather than becoming CRTP ---

The obvious objection to a virtual call in a hot path does not apply here,
because of where the call actually sits. MatchingLoop accumulates a whole
command's output into m_out and publishes ONCE, so this is one indirect call
per COMMAND, not per event — amortized over a batch that already did hash
lookups, engine matching and a ring push. It does not show up.

Templating the loop on its sink to remove it would cost real things:

  - MatchingLoop becomes a template, so MatchingThread does too, and both
    move into headers.
  - ServerConfig::egress selects "ring" or "post" at RUNTIME, and net_smoke
    uses VectorEventSink, so all three instantiations would have to exist —
    three copies of the entire matching loop, competing for icache, for a
    call that costs nothing.
  - The seam above, which is the most valuable testability property in this
    layer, would survive only as a concept rather than as something the build
    graph enforces.

The dispatch count is already minimal. The only way to reduce it further
would be to publish across several commands at once, which would break both
kEndOfBatch's per-command meaning and the guarantee that a batch is small
enough for the egress ring to accept whole.
*/
class EventSink {
 public:
  EventSink() = default;
  EventSink(const EventSink&) = delete;
  EventSink& operator=(const EventSink&) = delete;
  EventSink(EventSink&&) = delete;
  EventSink& operator=(EventSink&&) = delete;
  virtual ~EventSink() = default;

  virtual void publish(std::span<const Event> events) = 0;
};

// The test sink: keeps everything, in order, for assertions.
class VectorEventSink final : public EventSink {
 public:
  void publish(std::span<const Event> events) override {
    m_events.insert(m_events.end(), events.begin(), events.end());
  }

  const std::vector<Event>& events() const noexcept { return m_events; }
  void clear() noexcept { m_events.clear(); }

 private:
  std::vector<Event> m_events{};
};
}  // namespace Exchange::Net
