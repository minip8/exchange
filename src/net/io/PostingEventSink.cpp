#include "net/io/PostingEventSink.hpp"

#include <boost/asio/post.hpp>

namespace Exchange::Net {
void PostingEventSink::publish(std::span<const Event> events) {
  for (auto& bucket : m_buckets) bucket.clear();

  for (const Event& event : events) {
    const std::size_t thread{event.session_id >> 24};
    // An event addressed to a thread that does not exist can only come from
    // a corrupted session id; dropping it is strictly better than indexing
    // out of bounds.
    if (thread < m_buckets.size()) m_buckets[thread].push_back(event);
  }

  for (std::size_t i{0}; i < m_buckets.size(); ++i) {
    if (m_buckets[i].empty()) continue;
    IoThread* thread{m_threads[i].get()};
    asio::post(thread->context(),
               [thread, batch = m_buckets[i]]() { thread->dispatch(batch); });
  }
}
}  // namespace Exchange::Net
