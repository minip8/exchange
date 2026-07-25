#pragma once

#include <memory>
#include <span>
#include <vector>

#include "net/core/Event.hpp"
#include "net/gateway/EventSink.hpp"
#include "net/io/IoThread.hpp"

namespace Exchange::Net {
/*
The EventSink the matching thread publishes through once the egress rings
exist.

No allocation, no lock, no handler construction: bucket by destination I/O
thread (the high byte of the session id, an array index), push, and raise one
eventfd wake per thread per batch.

--- Backpressure: four conditions, four different answers ---

Full egress is not one situation, and treating it as one would be the actual
bug here. This class implements two of the four; the other two live on the
ingress side (SessionPump and IoThread::onIngressFull).

  private stream full   Disconnect. A dropped exec report silently
                        desynchronizes the client's position, and the client
                        has no way to notice. Resting orders survive.

  market data full      Drop it. Every message carries md_seq, so a client
                        detects the gap and asks for a snapshot. That is what
                        snapshot+delta is FOR, and dropping here means the
                        recovery path is exercised rather than dead code.

Note what this implies: an oversized batch is not simply rejected. It is
re-tried event by event so that the private messages in it still land and
only the market-data ones are sacrificed — which is the whole point of
distinguishing them.
*/
class RingEgressSink final : public EventSink {
 public:
  RingEgressSink(std::vector<std::unique_ptr<IoThread>>& threads,
                 std::size_t thread_count)
      : m_threads(threads), m_buckets(thread_count) {}

  void publish(std::span<const Event> events) override;

  uint64_t marketDataDrops() const noexcept { return m_md_drops; }
  uint64_t disconnects() const noexcept { return m_disconnects; }

 private:
  static bool isMarketData(EventType type) noexcept {
    return type == EventType::SnapshotBegin || type == EventType::LevelUpdate ||
           type == EventType::SnapshotEnd || type == EventType::TradePrint;
  }

  std::vector<std::unique_ptr<IoThread>>& m_threads;
  // Reused across calls; publish only ever runs on the matching thread, so
  // the steady state allocates nothing at all.
  std::vector<std::vector<Event>> m_buckets;
  uint64_t m_md_drops{0};
  uint64_t m_disconnects{0};
};
}  // namespace Exchange::Net
