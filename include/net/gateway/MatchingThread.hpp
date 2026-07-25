#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "net/core/Command.hpp"
#include "net/core/SpscRing.hpp"
#include "net/gateway/EventSink.hpp"
#include "net/gateway/MatchingLoop.hpp"

namespace Exchange::Net {
using IngressRing = SpscRing<Command, kIngressRingCapacity>;

/*
The matching thread: one ingress ring per I/O thread, round-robined.

--- Why per-thread SPSC rings and not one MPSC queue ---

The only ordering guarantee that matters is per-session FIFO, and a session
is pinned to one I/O thread for its whole life, so a single-producer ring
preserves it by construction. An MPSC queue would give a stronger guarantee
(a global order across sessions) that nothing needs.

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
