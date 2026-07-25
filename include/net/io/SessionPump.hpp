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
#include "net/core/RejectCode.hpp"
#include "net/io/IoThread.hpp"

namespace Exchange::Net {
/*
The ingress half of a client session, shared by the binary and WebSocket
transports.

Both need exactly the same thing — get a Command into this thread's ring,
never block if it is full, never reorder — and getting that wrong is the kind
of bug that shows up as one client's orders arriving out of sequence under
load. One implementation, used twice.
*/
class SessionPump {
 public:
  SessionPump(IngressRing& ring, asio::any_io_executor executor,
              std::function<void()> on_resume)
      : m_ring(ring),
        m_timer(std::move(executor)),
        m_on_resume(std::move(on_resume)) {}

  // Pushes, or queues and suspends reads if the ring is full.
  void submit(const Command& command) {
    if (command.type == CommandType::None) return;
    // Anything already queued goes first, or this session's commands would
    // reorder relative to each other. Per-session FIFO is the one ordering
    // guarantee the design makes, and the reason a single-producer ring per
    // I/O thread suffices.
    if (m_pending.empty() && m_ring.tryPush(command)) return;

    m_pending.push_back(command);
    if (!m_suspended) {
      m_suspended = true;
      retry();
    }
  }

  bool readsSuspended() const noexcept { return m_suspended; }

  void cancel() {
    m_cancelled = true;
    m_timer.cancel();
  }

 private:
  void retry() {
    while (!m_pending.empty() && m_ring.tryPush(m_pending.front())) {
      m_pending.pop_front();
    }
    if (m_pending.empty()) {
      m_suspended = false;
      if (m_on_resume) m_on_resume();
      return;
    }
    m_timer.expires_after(std::chrono::microseconds{200});
    m_timer.async_wait([this](const boost::system::error_code& ec) {
      if (ec || m_cancelled) return;
      retry();
    });
  }

  IngressRing& m_ring;
  std::deque<Command> m_pending{};
  asio::steady_timer m_timer;
  std::function<void()> m_on_resume;
  bool m_suspended{false};
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
