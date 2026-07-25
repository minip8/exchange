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
