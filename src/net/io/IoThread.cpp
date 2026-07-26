#include "net/io/IoThread.hpp"

#include <chrono>

#include "net/io/Log.hpp"
#include "net/io/TcpSession.hpp"

namespace Exchange::Net {
void IoThread::start() {
  if (m_thread.joinable()) return;
  // Arm the egress wake before the reactor runs, so an event published
  // between here and the first socket read is not stranded in the ring.
  m_egress.start([this](std::span<const Event> events) { dispatch(events); },
                 [this](uint32_t session_id) {
                   if (auto session{m_sessions.find(session_id)}) {
                     session->close("egress overflow on a private stream");
                   }
                 });
  m_thread = std::thread{[this] {
    try {
      m_context.run();
    } catch (const std::exception& error) {
      logError("io thread {} died: {}", m_index, error.what());
    }
  }};
}

void IoThread::stop() {
  m_egress.stop();
  m_guard.reset();
  m_context.stop();
}

void IoThread::join() {
  if (m_thread.joinable()) m_thread.join();
}

void IoThread::dispatch(std::span<const Event> events) {
  // Events arrive already ordered and, in the common case, already grouped:
  // the matching thread emits a whole command's worth at once, and most of
  // those go to one session. Walking the run rather than building a map
  // keeps this allocation-free.
  std::size_t start{0};
  while (start < events.size()) {
    const uint32_t session_id{events[start].session_id};
    std::size_t end{start + 1};
    while (end < events.size() && events[end].session_id == session_id) ++end;

    if (auto session{m_sessions.find(session_id)}) {
      session->deliver(events.subspan(start, end - start));
    }
    // A missing session is normal, not an error: it disconnected while the
    // matching thread was producing events for it.
    start = end;
  }
}

void IoThread::onIngressFull() {
  if (m_reads_suspended) return;
  m_reads_suspended = true;
  ++m_suspensions;
  checkIngressPressure();
}

void IoThread::checkIngressPressure() {
  if (m_ring.freeSlots() >= kResumeFreeSlots) {
    m_reads_suspended = false;
    // all() snapshots, so a session closing inside resumeReads cannot
    // invalidate the iteration.
    for (const auto& session : m_sessions.all()) session->resumeReads();
    return;
  }
  m_pressure_timer.expires_after(std::chrono::microseconds{200});
  m_pressure_timer.async_wait([this](const boost::system::error_code& ec) {
    if (ec) {
      m_reads_suspended = false;
      return;
    }
    checkIngressPressure();
  });
}

void IoThread::enqueueCritical(const Command& command) {
  m_critical.push_back(command);
  if (!m_draining) {
    m_draining = true;
    drainCritical();
  }
}

void IoThread::drainCritical() {
  while (!m_critical.empty() && m_ring.tryPush(m_critical.front())) {
    m_critical.pop_front();
  }
  if (m_critical.empty()) {
    m_draining = false;
    return;
  }
  m_critical_timer.expires_after(std::chrono::microseconds{200});
  m_critical_timer.async_wait([this](const boost::system::error_code& ec) {
    if (ec) {
      m_draining = false;
      return;
    }
    drainCritical();
  });
}
}  // namespace Exchange::Net
