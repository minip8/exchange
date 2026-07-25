#pragma once

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <cstdint>
#include <functional>
#include <span>

#include "net/core/Event.hpp"
#include "net/core/SpscRing.hpp"

namespace Exchange::Net {
namespace asio = boost::asio;

using EgressRing = SpscRing<Event, kEgressRingCapacity>;

/*
Egress, stage two: one SPSC ring per I/O thread plus an eventfd to wake it.

This replaces the asio::post path, which allocated a handler and took the
io_context's internal lock for every event — quietly reintroducing exactly
the two costs the ingress ring exists to avoid. net_loopback_bench exists to
show the difference rather than assert it.

--- The wake protocol, which is the part that is easy to get wrong ---

An eventfd rather than a condition variable or a self-pipe, because
asio::posix::stream_descriptor makes it a first-class epoll source: the I/O
thread waits on its sockets and on this in the same reactor, with no extra
thread and no polling.

`m_notified` is what keeps the syscall count down. A producer only writes to
the eventfd if it is the one that flipped the flag from false to true, so a
burst of a thousand events costs ONE write, not a thousand.

The ordering is the subtle bit. On wake the consumer must:

    1. read() the eventfd, clearing the readable state;
    2. clear m_notified, so producers start signalling again;
    3. THEN drain the ring.

Steps 2 and 3 in that order, not the other way round. If the ring were
drained first, a producer could push between the drain and the clear, see
m_notified still set, skip its write — and its event would sit in the ring
with nothing scheduled to come back for it. Clearing first means any producer
that pushes from that moment on is guaranteed to raise a new wake.

The `if (!empty()) selfWake()` after draining closes the same race from the
other side and costs one branch on an already-cold path.
*/
class EgressQueue {
 public:
  using Drain = std::function<void(std::span<const Event>)>;

  explicit EgressQueue(asio::io_context& context);
  EgressQueue(const EgressQueue&) = delete;
  EgressQueue& operator=(const EgressQueue&) = delete;
  ~EgressQueue();

  // ---- producer side (matching thread) ----

  // All-or-nothing, so a snapshot crosses intact.
  [[nodiscard]] bool pushBatch(std::span<const Event> events) noexcept {
    return m_ring.tryPushBatch(events);
  }
  [[nodiscard]] bool push(const Event& event) noexcept {
    return m_ring.tryPush(event);
  }

  // One eventfd write per burst, no matter how many events it carried.
  void notify() noexcept;

  /*
  A session whose PRIVATE stream overflowed, queued for disconnection.

  Dropping an exec report silently desynchronizes a client's view of its own
  position and P&L, and it has no way to detect that it happened. Real
  exchanges kill slow private sessions for exactly this reason. Resting
  orders survive the disconnect, so nothing is lost but the connection.
  */
  void requestDisconnect(uint32_t session_id) noexcept;

  /*
  A dropped market-data message, which is a different situation entirely.

  Market data is idempotent-recoverable: every message carries md_seq, and a
  client that sees a gap asks for a fresh snapshot. That recovery path exists
  precisely so this drop is survivable — and dropping here means the path
  gets exercised rather than rotting unused.
  */
  void recordMarketDataDrop() noexcept {
    m_md_drops.fetch_add(1, std::memory_order_relaxed);
  }

  uint64_t marketDataDrops() const noexcept {
    return m_md_drops.load(std::memory_order_relaxed);
  }
  uint64_t wakeups() const noexcept {
    return m_wakeups.load(std::memory_order_relaxed);
  }
  uint64_t eventsDrained() const noexcept {
    return m_drained.load(std::memory_order_relaxed);
  }

  // ---- consumer side (I/O thread) ----

  // `drain` is called with a run of events for ONE session at a time.
  // `disconnect` is called for sessions whose private stream overflowed.
  void start(Drain drain, std::function<void(uint32_t)> disconnect);
  void stop();

 private:
  void arm();
  void onWake();

  // 64 at a time: enough to amortize the ring's atomics over a burst, small
  // enough that one session's flood cannot hold the reactor for long.
  static constexpr std::size_t kDrainBatch{64};

  EgressRing m_ring{};
  // Session ids whose private stream overflowed. Bounded and lossy on
  // purpose: if this fills, the sessions in it are already being killed.
  SpscRing<uint32_t, 64> m_disconnects{};

  asio::posix::stream_descriptor m_descriptor;
  std::atomic<bool> m_notified{false};
  std::atomic<bool> m_stopped{false};
  std::atomic<uint64_t> m_md_drops{0};
  std::atomic<uint64_t> m_wakeups{0};
  std::atomic<uint64_t> m_drained{0};
  uint64_t m_counter{0};  // eventfd read target; consumer-private

  Drain m_drain{};
  std::function<void(uint32_t)> m_disconnect{};
};
}  // namespace Exchange::Net
