#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>

#include "net/core/Command.hpp"
#include "net/core/Event.hpp"
#include "net/core/RejectCode.hpp"
#include "net/io/IoThread.hpp"

namespace Exchange::Net {
/*
The ingress half of a client session, shared by the binary and WebSocket
transports.

Both need exactly the same thing — get a Command into this thread's ring,
never block if it is full, never reorder — and getting that wrong shows up as
one client's orders arriving out of sequence under load. One implementation,
used twice.

--- On the in-flight counter being a plain uint32_t ---

It is incremented when a command is submitted and decremented when the event
carrying kCommandComplete comes back, and it is NOT atomic. That is not an
oversight: a session is pinned to one I/O thread for its whole life, so the
submit side and the completion side are literally the same thread. This is
the concrete payoff of thread-per-io_context — the alternative shape, one
context shared by N threads, would need this atomic (or strand-guarded)
purely because a completion could land on any of them.
*/
class SessionPump {
 public:
  // How many commands a session may have outstanding before it is throttled.
  // Generous for a human, immediately binding for a runaway loop, which is
  // the only thing this defends against.
  static constexpr uint32_t kMaxInFlight{64};

  SessionPump(IoThread& io, asio::any_io_executor executor,
              std::function<void()> on_resume)
      : m_io(io),
        m_timer(std::move(executor)),
        m_on_resume(std::move(on_resume)) {}

  // Reserves a credit. Returns false if the session is at its limit, in
  // which case the caller must reject with Throttled rather than queue —
  // queueing would only move the flood one layer down.
  [[nodiscard]] bool tryReserve() noexcept {
    if (m_in_flight >= kMaxInFlight) return false;
    ++m_in_flight;
    return true;
  }

  void onCommandComplete() noexcept {
    if (m_in_flight > 0) --m_in_flight;
  }

  uint32_t inFlight() const noexcept { return m_in_flight; }

  // Pushes, or queues and asks the thread to stop reading if the ring is
  // full. A reserved credit is released here when the command turns out to
  // be a no-op, so `None` cannot leak one.
  void submit(const Command& command) {
    if (command.type == CommandType::None) {
      onCommandComplete();
      return;
    }
    // Anything already queued goes first, or this session's commands would
    // reorder relative to each other. Per-session FIFO is the one ordering
    // guarantee the design makes, and the reason a single-producer ring per
    // I/O thread is sufficient.
    if (m_pending.empty() && m_io.ring().tryPush(command)) return;

    m_pending.push_back(command);
    /*
    The ring is a THREAD-wide resource, not a session-wide one, so a session
    that fills it is starving its neighbours and not only itself. Suspending
    every session on the thread is the honest response: TCP backpressure then
    reaches all of them, which is a signal clients can actually act on.
    */
    m_io.onIngressFull();
    if (!m_draining) {
      m_draining = true;
      retry();
    }
  }

  void cancel() {
    m_cancelled = true;
    m_timer.cancel();
  }

 private:
  void retry() {
    while (!m_pending.empty() && m_io.ring().tryPush(m_pending.front())) {
      m_pending.pop_front();
    }
    if (m_pending.empty()) {
      m_draining = false;
      if (m_on_resume) m_on_resume();
      return;
    }
    m_timer.expires_after(std::chrono::microseconds{200});
    m_timer.async_wait([this](const boost::system::error_code& ec) {
      if (ec || m_cancelled) return;
      retry();
    });
  }

  IoThread& m_io;
  std::deque<Command> m_pending{};
  asio::steady_timer m_timer;
  std::function<void()> m_on_resume;
  uint32_t m_in_flight{0};
  bool m_draining{false};
  bool m_cancelled{false};
};

struct AuthOutcome {
  bool ok{false};
  uint32_t trader_id{0};
  RejectCode code{RejectCode::AuthFailed};
  std::string name{};
};

/*
Shared by both transports so there is exactly one place where credentials are
checked. Runs on the I/O thread against the immutable TraderDirectory, and
returns only the resolved identity — the key itself never travels further.
*/
inline AuthOutcome authenticateSession(const TraderDirectory& traders,
                                       LogonAttemptLimiter& limiter,
                                       uint32_t peer_ip,
                                       std::string_view api_key,
                                       uint64_t now_ns) {
  if (!limiter.allowed(peer_ip, now_ns)) {
    return AuthOutcome{.ok = false,
                       .trader_id = 0,
                       .code = RejectCode::AuthFailed,
                       .name = {}};
  }
  const Trader* trader{traders.authenticate(api_key)};
  if (trader == nullptr) {
    limiter.recordFailure(peer_ip, now_ns);
    return AuthOutcome{.ok = false,
                       .trader_id = 0,
                       .code = RejectCode::AuthFailed,
                       .name = {}};
  }
  limiter.recordSuccess(peer_ip);
  return AuthOutcome{.ok = true,
                     .trader_id = trader->id,
                     .code = RejectCode::None,
                     .name = trader->name};
}
}  // namespace Exchange::Net
