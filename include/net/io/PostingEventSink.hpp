#pragma once

#include <memory>
#include <span>
#include <vector>

#include "net/core/Event.hpp"
#include "net/gateway/EventSink.hpp"
#include "net/io/IoThread.hpp"

namespace Exchange::Net {
/*
Egress, stage one: asio::post from the matching thread to the owning I/O
thread.

This is knowingly the wrong shape for the long run. Every post allocates a
handler and takes the io_context's internal lock, which quietly reintroduces
exactly the two costs the ingress ring exists to avoid. At <20 clients it is
unmeasurable, and it gets a working exchange running several phases earlier,
which is the trade being made.

Phase 6 replaces this with per-I/O-thread egress rings and an eventfd wake,
and the loopback benchmark exists to show the difference rather than assert
it.

Routing is O(1): the high byte of a session id is its I/O thread index.
*/
class PostingEventSink final : public EventSink {
 public:
  // The thread count is passed separately because the sink is constructed
  // before the I/O threads are: MatchingThread owns the ingress rings, and
  // an IoThread needs its ring at construction, so the order is
  // sink -> matching thread -> I/O threads. The vector reference is only
  // dereferenced inside publish(), by which time it is populated.
  PostingEventSink(std::vector<std::unique_ptr<IoThread>>& threads,
                   std::size_t thread_count)
      : m_threads(threads), m_buckets(thread_count) {}

  void publish(std::span<const Event> events) override;

 private:
  std::vector<std::unique_ptr<IoThread>>& m_threads;
  // Reused across calls: publish is only ever called from the matching
  // thread, so the steady state does not allocate for the bucketing itself
  // (the per-post copy still does — see above).
  std::vector<std::vector<Event>> m_buckets;
};
}  // namespace Exchange::Net
