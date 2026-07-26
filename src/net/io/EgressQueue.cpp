#include "net/io/EgressQueue.hpp"

#include <sys/eventfd.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <stdexcept>

namespace Exchange::Net {
namespace {
int makeEventFd() {
  // Non-blocking so a spurious wake cannot park the I/O thread in read(),
  // and close-on-exec because a descriptor leaking into a child is the kind
  // of thing nobody notices until it matters.
  const int fd{::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)};
  if (fd < 0) throw std::runtime_error{"eventfd: out of descriptors"};
  return fd;
}
}  // namespace

EgressQueue::EgressQueue(asio::io_context& context)
    : m_descriptor(context, makeEventFd()) {}

EgressQueue::~EgressQueue() { stop(); }

void EgressQueue::notify() noexcept {
  // The whole point: only the producer that flips false -> true pays for a
  // syscall. A burst of a thousand events costs one write.
  if (m_notified.exchange(true, std::memory_order_acq_rel)) return;
  const uint64_t one{1};
  // Errors here are not recoverable and not worth branching on: EAGAIN can
  // only happen if the counter is saturated at 2^64-2, which would mean the
  // consumer has not run in a geological age.
  [[maybe_unused]] const ssize_t written{
      ::write(m_descriptor.native_handle(), &one, sizeof(one))};
}

void EgressQueue::requestDisconnect(uint32_t session_id) noexcept {
  static_cast<void>(m_disconnects.tryPush(session_id));
}

void EgressQueue::start(Drain drain, std::function<void(uint32_t)> disconnect) {
  m_drain = std::move(drain);
  m_disconnect = std::move(disconnect);
  arm();
}

void EgressQueue::stop() {
  if (m_stopped.exchange(true, std::memory_order_acq_rel)) return;
  boost::system::error_code ec{};
  m_descriptor.close(ec);
}

void EgressQueue::arm() {
  if (m_stopped.load(std::memory_order_acquire)) return;
  m_descriptor.async_wait(asio::posix::stream_descriptor::wait_read,
                          [this](const boost::system::error_code& ec) {
                            if (ec) return;  // closed during shutdown
                            onWake();
                          });
}

void EgressQueue::onWake() {
  m_wakeups.fetch_add(1, std::memory_order_relaxed);

  // 1. Clear the descriptor's readable state.
  while (::read(m_descriptor.native_handle(), &m_counter, sizeof(m_counter)) >
         0) {
    // eventfd returns the accumulated count in one read; the loop only
    // matters if a producer wrote again mid-read.
  }

  // 2. Clear the flag BEFORE draining. See the header: draining first would
  // let a producer push into the gap, observe the flag still set, skip its
  // write, and leave its event sitting in the ring with no wake scheduled.
  m_notified.store(false, std::memory_order_release);

  // 3. Now drain.
  std::array<Event, kDrainBatch> batch{};
  std::size_t total{0};
  for (;;) {
    const std::size_t count{m_ring.tryPopBatch(batch)};
    if (count == 0) break;
    total += count;

    /*
    Group consecutive events by session and hand each session ONE run.

    This is where the real syscall saving lives — more than the ring itself.
    The matching thread emits a whole command's worth of events at once and
    most of them are for the same client, so grouping turns a burst into one
    async_write per session per drain rather than one per event.
    */
    std::size_t start{0};
    while (start < count) {
      const uint32_t session_id{batch[start].session_id};
      std::size_t end{start + 1};
      while (end < count && batch[end].session_id == session_id) ++end;
      m_drain(std::span<const Event>{batch.data() + start, end - start});
      start = end;
    }

    if (count < kDrainBatch) break;
  }
  if (total != 0) m_drained.fetch_add(total, std::memory_order_relaxed);

  // Sessions whose private stream overflowed go now, after their surviving
  // events have been written, so the client sees as much as we managed to
  // deliver before the connection drops.
  // Requests repeat while a session stays over its limit, so the callback —
  // not this loop — decides whether there is still a session to close, and
  // logs only then.
  uint32_t session_id{0};
  while (m_disconnects.tryPop(session_id)) {
    if (m_disconnect) m_disconnect(session_id);
  }

  // Belt and braces for the other side of the same race: a producer that
  // pushed just before step 2 would have seen the flag set and skipped its
  // write. The drain above should already have picked it up, but a
  // self-wake here is one branch on a cold path against a permanently
  // stalled event.
  if (!m_ring.empty()) {
    const uint64_t one{1};
    [[maybe_unused]] const ssize_t written{
        ::write(m_descriptor.native_handle(), &one, sizeof(one))};
  }

  arm();
}
}  // namespace Exchange::Net
