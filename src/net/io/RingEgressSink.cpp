#include "net/io/RingEgressSink.hpp"

#include "net/io/EgressQueue.hpp"

namespace Exchange::Net {
/*
The batch did not fit. Rather than dropping it whole, place what will fit and
apply the per-stream policy to the rest: private messages are worth a
disconnect, market data is worth a gap the client can recover from. Doing
this the other way round — dropping the whole batch — would lose exec reports
to save level updates, which is backwards.
*/
void RingEgressSink::applyOverflowPolicy(EgressQueue& queue,
                                         std::span<const Event> events) {
  // One disconnect request per session per batch, not per event: a session
  // that overflows once usually overflows for every remaining event in the
  // batch, and queueing dozens of requests for the same doomed session would
  // just crowd out the others.
  uint32_t last_disconnected{0};
  for (const Event& event : events) {
    if (queue.push(event)) continue;
    if (isMarketData(event.type)) {
      queue.recordMarketDataDrop();
      ++m_md_drops;
    } else if (event.session_id != last_disconnected) {
      last_disconnected = event.session_id;
      queue.requestDisconnect(event.session_id);
      ++m_disconnects;
    }
  }
}

void RingEgressSink::publish(std::span<const Event> events) {
  if (events.empty()) return;

  /*
  Fast path: the whole batch shares one destination thread, so the bucket
  vectors are pure overhead — every event would be copied in and immediately
  copied back out. Push the caller's span straight at the ring instead.

  At the default io_threads=1 this is every batch. Even with several threads
  it is most of them, because a batch is one command's output and a command's
  acks and exec reports all belong to sessions the command already named.
  */
  const std::size_t first{events.front().session_id >> 24};
  bool uniform{first < m_buckets.size()};
  if (uniform) {
    for (const Event& event : events) {
      if ((event.session_id >> 24) != first) {
        uniform = false;
        break;
      }
    }
  }

  if (uniform) {
    EgressQueue& queue{m_threads[first]->egress()};
    if (!queue.pushBatch(events)) applyOverflowPolicy(queue, events);
    queue.notify();
    return;
  }

  for (auto& bucket : m_buckets) bucket.clear();

  for (const Event& event : events) {
    const std::size_t thread{event.session_id >> 24};
    // An event addressed to a thread that does not exist can only come from
    // a corrupted session id; dropping it beats indexing out of bounds.
    if (thread < m_buckets.size()) m_buckets[thread].push_back(event);
  }

  for (std::size_t i{0}; i < m_buckets.size(); ++i) {
    if (m_buckets[i].empty()) continue;
    EgressQueue& queue{m_threads[i]->egress()};
    if (!queue.pushBatch(m_buckets[i])) {
      applyOverflowPolicy(queue, m_buckets[i]);
    }
    queue.notify();
  }
}
}  // namespace Exchange::Net
