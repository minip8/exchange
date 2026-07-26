#pragma once

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <cstdint>
#include <deque>
#include <span>
#include <thread>

#include "net/core/Command.hpp"
#include "net/core/Event.hpp"
#include "net/core/SpscRing.hpp"
#include "net/core/Tuning.hpp"
#include "net/io/EgressQueue.hpp"
#include "net/io/SessionTable.hpp"
#include "net/io/TraderDirectory.hpp"

namespace Exchange::Net {
namespace asio = boost::asio;

using IngressRing = SpscRing<Command, kIngressRingCapacity>;

/*
One io_context per thread, not one context shared by N threads.

The consequence is the point: a session's socket is only ever touched by its
own thread, so there are NO STRANDS ANYWHERE. No completion-handler
serialization, no implicit queueing, and per-session state (write buffers,
frame parser, credit counters) can be plain non-atomic members.

Sessions are numbered (index << 24) | local_counter. The high byte is the
owning thread, which makes egress routing an O(1) array index rather than a
lookup, and the per-thread counter means no atomic and no global registry.
*/
class IoThread {
 public:
  IoThread(std::size_t index, IngressRing& ring)
      : m_index(index), m_ring(ring) {}
  IoThread(const IoThread&) = delete;
  IoThread& operator=(const IoThread&) = delete;

  asio::io_context& context() noexcept { return m_context; }
  std::size_t index() const noexcept { return m_index; }
  SessionTable& sessions() noexcept { return m_sessions; }
  LogonAttemptLimiter& limiter() noexcept { return m_limiter; }
  IngressRing& ring() noexcept { return m_ring; }
  EgressQueue& egress() noexcept { return m_egress; }

  // Session 0 is never handed out: 0 is the "no session" sentinel used
  // throughout the gateway.
  uint32_t nextSessionId() noexcept {
    return (static_cast<uint32_t>(m_index) << 24) | (m_next_local++);
  }

  void start();
  void stop();
  void join();

  // Fans a batch of events out to their destination sessions. Runs on this
  // thread, posted by the matching thread. Consecutive events for the same
  // session are grouped so each session sees one deliver() call — the
  // beginning of the write coalescing that Phase 6 completes.
  void dispatch(std::span<const Event> events);

  /*
  A command that may never be dropped, queued because the ingress ring was
  full at the moment it was produced.

  Only SessionClosed uses this today. It cannot live on the session (the
  session is being destroyed) and it cannot spin (that would stall every
  other session on this thread), so the thread itself owns the retry.
  */
  void enqueueCritical(const Command& command);

  /*
  Ingress backpressure, applied to the whole thread.

  The ingress ring belongs to the thread, not to a session, so one session
  filling it starves its neighbours. Reads stop on every session here until
  the ring has drained to a low-water mark, which turns the overload into TCP
  backpressure — the one signal a client cannot ignore or misinterpret.
  */
  void onIngressFull();
  bool readsSuspended() const noexcept { return m_reads_suspended; }
  uint64_t suspensions() const noexcept { return m_suspensions; }

 private:
  void drainCritical();
  void checkIngressPressure();

  std::size_t m_index;
  IngressRing& m_ring;
  asio::io_context m_context{1};
  // Without this, run() returns the instant the context runs dry — which it
  // does between the listener being armed and the first connection arriving.
  asio::executor_work_guard<asio::io_context::executor_type> m_guard{
      asio::make_work_guard(m_context)};
  uint32_t m_next_local{1};
  SessionTable m_sessions{};
  LogonAttemptLimiter m_limiter{};
  EgressQueue m_egress{m_context};
  std::deque<Command> m_critical{};
  // Resume only once the ring is a quarter empty, not the instant one slot
  // frees: resuming at the first free slot would re-fill it immediately and
  // spend the thread's time toggling reads on and off.
  static constexpr std::size_t kResumeFreeSlots{kIngressRingCapacity / 4};
  asio::steady_timer m_pressure_timer{m_context};
  bool m_reads_suspended{false};
  uint64_t m_suspensions{0};
  asio::steady_timer m_critical_timer{m_context};
  bool m_draining{false};
  std::thread m_thread{};
};
}  // namespace Exchange::Net
