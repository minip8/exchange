#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "net/core/Command.hpp"
#include "net/core/SpscRing.hpp"
#include "net/core/Tuning.hpp"
#include "net/gateway/EventSink.hpp"
#include "net/gateway/MatchingLoop.hpp"

namespace Exchange::Net {
using IngressRing = SpscRing<Command, kIngressRingCapacity>;

/*
The matching thread: one ingress ring per I/O thread, round-robined.

--- What is guaranteed, and what is not ---

Guaranteed: PER-SESSION FIFO. A session is pinned to one I/O thread for its
whole life and therefore to one ring, and a ring preserves the order its
single producer wrote in. Nothing can reorder one client's own commands.

NOT guaranteed, and this is the part worth stating explicitly rather than
leaving as an absence: there is NO cross-session time priority. If two
clients submit at the same instant, nothing here decides which the book sees
first on the basis of when they actually arrived.

  - Same I/O thread: both land in one ring in whatever order that thread
    happened to read their sockets, which is epoll wake order.
  - Different I/O threads: run() drains up to kBatchCap from one ring before
    it so much as looks at the next, so the loser can be 64 commands behind.

kBatchCap is the knob that bounds that skew, and lowering it is the only
zero-code way to tighten it.

Ordering by Command::recv_ts_ns — peeking each ring's head and taking the
oldest — was considered and rejected, because it would narrow the skew
without making anything deterministic:

  - The merge sees only ring HEADS. An empty ring may be about to receive an
    older command, and nothing can know that without blocking, so the outcome
    would still turn on when the consumer happened to look.
  - recv_ts_ns is stamped when an I/O thread reads bytes off a socket, not
    when the packet arrived. Epoll wake order, kernel socket buffering, and
    how many other sessions that thread was already servicing all sit between
    arrival and the stamp — so it would order by "when my I/O thread got to
    me", not by arrival.
  - nowNs() is a system_clock, so an NTP step could invert the comparison.

Real cross-session time priority needs NIC hardware timestamping
(SO_TIMESTAMPING) or a sequencer holding commands in a bounded reorder window
before release. Both are much larger than this loop and add latency by
design, so neither is here.

None of this touches the determinism claim in MatchingLoop.hpp: a scripted
vector<Command> still produces a bit-identical vector<Event>, because that is
a property of the loop, and the nondeterminism above lives upstream of it in
what order the I/O threads produce commands at all.

--- Why per-thread SPSC rings and not one MPSC queue ---

The only ordering guarantee that matters is per-session FIFO, and a session
is pinned to one I/O thread for its whole life, so a single-producer ring
preserves it by construction. An MPSC queue would give a stronger guarantee
(a global order across sessions) that nothing needs — and, per the above, not
even an MPSC queue would give true arrival-time priority.

The implementation cost is not close. SPSC publication is a plain release
store; a bounded MPSC needs a Vyukov ticket and per-slot sequence numbers,
which is a genuinely subtle thing to get right and an unpleasant thing to
debug. Writing the textbook ring correctly is the point of the exercise;
debugging a bounded MPSC queue is not.

Round-robin with a per-ring batch cap is also an explicit, measurable
fairness knob, which a single queue would not give.

The cost is that an idle poll is N acquire loads instead of 1. At N=2 that is
noise. If N ever gets large enough for it to matter, that is the moment to
revisit — and the tradeoff is written down here so the decision can be
re-made with the numbers in hand rather than re-derived.
*/
class MatchingThread {
 public:
  // How many commands are taken from one ring before moving to the next.
  // This is the fairness knob: too small and the round-robin overhead
  // dominates, too large and one busy I/O thread can starve the others.
  static constexpr std::size_t kBatchCap{64};

  MatchingThread(EventSink& sink, std::size_t io_thread_count,
                 MatchingLoopConfig config = {}, uint32_t spin_us = 0);
  MatchingThread(const MatchingThread&) = delete;
  MatchingThread& operator=(const MatchingThread&) = delete;
  ~MatchingThread();

  // The producer end for I/O thread `index`. Only that thread may push.
  IngressRing& ring(std::size_t index) { return *m_rings[index]; }

  void start();
  void stop();

  // Only safe to call after stop(), or from the matching thread itself.
  const MatchingLoop& loop() const noexcept { return m_loop; }

  uint64_t commandsHandled() const noexcept {
    return m_commands_handled.load(std::memory_order_relaxed);
  }

 private:
  void run();

  MatchingLoop m_loop;
  // Heap-allocated so the alignas(64) inside the ring is honoured — a vector
  // of over-aligned objects would otherwise depend on the allocator.
  std::vector<std::unique_ptr<IngressRing>> m_rings{};
  std::atomic<bool> m_stop{false};
  std::atomic<uint64_t> m_commands_handled{0};
  uint32_t m_spin_us;
  std::thread m_thread{};
};
}  // namespace Exchange::Net
