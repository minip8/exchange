#include "net/gateway/MatchingThread.hpp"

#include <array>
#include <chrono>

#include "net/core/CacheLine.hpp"

namespace Exchange::Net {
MatchingThread::MatchingThread(EventSink& sink, std::size_t io_thread_count,
                               MatchingLoopConfig config, uint32_t spin_us)
    : m_loop(sink, config), m_spin_us(spin_us) {
  m_rings.reserve(io_thread_count);
  for (std::size_t i{0}; i < io_thread_count; ++i) {
    m_rings.push_back(std::make_unique<IngressRing>());
  }
}

MatchingThread::~MatchingThread() { stop(); }

void MatchingThread::start() {
  if (m_thread.joinable()) return;
  m_thread = std::thread{[this] { run(); }};
}

void MatchingThread::stop() {
  m_stop.store(true, std::memory_order_release);
  if (m_thread.joinable()) m_thread.join();
}

void MatchingThread::run() {
  std::array<Command, kBatchCap> batch{};
  const auto spin_duration{std::chrono::microseconds{m_spin_us}};
  auto idle_since{std::chrono::steady_clock::now()};

  while (!m_stop.load(std::memory_order_acquire)) {
    std::size_t handled{0};
    for (const auto& ring : m_rings) {
      const std::size_t n{ring->tryPopBatch(batch)};
      if (n == 0) continue;
      m_loop.handleBatch(std::span<const Command>{batch.data(), n});
      handled += n;
    }

    if (handled != 0) {
      m_commands_handled.fetch_add(handled, std::memory_order_relaxed);
      idle_since = std::chrono::steady_clock::now();
      continue;
    }

    /*
    Three-stage backoff. The contrast between the first and last stage is one
    of the more instructive numbers this project produces: spinning buys
    single-digit-microsecond wakeups at the price of a permanently hot core,
    which is why the default is 0 and benchmarking uses ~100.
    */
    const auto idle_for{std::chrono::steady_clock::now() - idle_since};
    if (idle_for < spin_duration) {
      cpuPause();
    } else if (idle_for < spin_duration + std::chrono::microseconds{200}) {
      std::this_thread::yield();
    } else {
      std::this_thread::sleep_for(std::chrono::microseconds{200});
    }
  }

  // Drain whatever the I/O threads pushed before the stop was observed, so a
  // clean shutdown does not silently swallow acknowledged commands.
  for (const auto& ring : m_rings) {
    for (;;) {
      const std::size_t n{ring->tryPopBatch(batch)};
      if (n == 0) break;
      m_loop.handleBatch(std::span<const Command>{batch.data(), n});
    }
  }
}
}  // namespace Exchange::Net
