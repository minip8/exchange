#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <span>
#include <type_traits>

#include "net/core/CacheLine.hpp"

namespace Exchange::Net {
/*
A bounded single-producer / single-consumer queue.

Indices are monotonic and never wrapped; only the *slot lookup* masks them.
That is what makes full-vs-empty unambiguous without sacrificing a slot:
`write - read` is the exact occupancy, and it is correct across the unsigned
wraparound of the counters themselves.

Ordering contract, precisely:

  - The producer's `release` store to `m_write` is the ONLY thing that
    publishes the slot writes preceding it. The consumer's matching `acquire`
    load of `m_write` is what makes them visible.
  - The consumer's `release` store to `m_read` publishes "I am done reading
    those slots". Without it the producer could overwrite a slot the consumer
    is still copying out — a write-after-read race that TSan flags and that
    real ARM hardware can actually expose. It is not decoration.
  - Both `relaxed` loads are a thread reading back its own index, which no
    other thread writes.

`m_cached_read`/`m_cached_write` are private to their own thread and live on
that thread's cache line. They let the common path avoid touching the other
thread's line at all: the producer only re-reads `m_read` when it *looks*
full, the consumer only re-reads `m_write` when it *looks* empty. That is the
entire point of the exercise — a ring without them ping-pongs a cache line
between cores on every single operation.

T must be trivially copyable: slots are plain storage, assigned over, never
constructed or destroyed. Command and Event are both exactly one cache line
and satisfy this by static_assert.
*/
template <typename T, std::size_t Capacity>
  requires std::is_trivially_copyable_v<T> && (std::has_single_bit(Capacity))
class SpscRing {
 public:
  static constexpr std::size_t kCapacity{Capacity};

  SpscRing() noexcept = default;
  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;
  SpscRing(SpscRing&&) = delete;
  SpscRing& operator=(SpscRing&&) = delete;
  ~SpscRing() noexcept = default;

  // ---- producer side ----

  [[nodiscard]] bool tryPush(const T& item) noexcept {
    const std::size_t w{m_write.load(std::memory_order_relaxed)};
    if (w - m_cached_read >= Capacity) {                       // maybe full
      m_cached_read = m_read.load(std::memory_order_acquire);  // re-sync once
      if (w - m_cached_read >= Capacity) return false;
    }
    m_slots[w & kMask] = item;
    m_write.store(w + 1, std::memory_order_release);  // publishes the slot
    return true;
  }

  /*
  All-or-nothing: either every item lands and becomes visible under one
  release store, or nothing does. That atomicity is what lets a market-data
  snapshot (SnapshotBegin + N LevelUpdates + SnapshotEnd) cross as a unit
  without any cross-thread ownership contract — the consumer can never
  observe a torn snapshot.
  */
  [[nodiscard]] bool tryPushBatch(std::span<const T> items) noexcept {
    if (items.empty()) return true;
    if (items.size() > Capacity) return false;
    const std::size_t w{m_write.load(std::memory_order_relaxed)};
    if (Capacity - (w - m_cached_read) < items.size()) {
      m_cached_read = m_read.load(std::memory_order_acquire);
      if (Capacity - (w - m_cached_read) < items.size()) return false;
    }
    for (std::size_t i{0}; i < items.size(); ++i) {
      m_slots[(w + i) & kMask] = items[i];
    }
    m_write.store(w + items.size(), std::memory_order_release);
    return true;
  }

  // Producer-side occupancy estimate. Conservative: it may under-report free
  // space (the cached read index can lag), never over-report.
  [[nodiscard]] std::size_t freeSlots() noexcept {
    const std::size_t w{m_write.load(std::memory_order_relaxed)};
    m_cached_read = m_read.load(std::memory_order_acquire);
    return Capacity - (w - m_cached_read);
  }

  // ---- consumer side ----

  [[nodiscard]] bool tryPop(T& out) noexcept {
    const std::size_t r{m_read.load(std::memory_order_relaxed)};
    if (r == m_cached_write) {  // maybe empty
      m_cached_write = m_write.load(std::memory_order_acquire);
      if (r == m_cached_write) return false;
    }
    out = m_slots[r & kMask];
    m_read.store(r + 1, std::memory_order_release);  // publishes "slot free"
    return true;
  }

  // Copies out at most `out.size()` items and returns how many. One acquire
  // load and one release store amortized over the whole batch.
  [[nodiscard]] std::size_t tryPopBatch(std::span<T> out) noexcept {
    if (out.empty()) return 0;
    const std::size_t r{m_read.load(std::memory_order_relaxed)};
    if (r == m_cached_write) {
      m_cached_write = m_write.load(std::memory_order_acquire);
      if (r == m_cached_write) return 0;
    }
    std::size_t count{m_cached_write - r};
    if (count > out.size()) count = out.size();
    for (std::size_t i{0}; i < count; ++i) out[i] = m_slots[(r + i) & kMask];
    m_read.store(r + count, std::memory_order_release);
    return count;
  }

  // Consumer-side emptiness check that does not consume. Cheap when the
  // cached index already proves there is work.
  [[nodiscard]] bool empty() noexcept {
    const std::size_t r{m_read.load(std::memory_order_relaxed)};
    if (r != m_cached_write) return false;
    m_cached_write = m_write.load(std::memory_order_acquire);
    return r == m_cached_write;
  }

  // ---- either side (diagnostics only) ----

  // Both loads are acquire, but they are not read atomically together, so
  // this is a snapshot of two moments. Fine for stats, not for logic.
  [[nodiscard]] std::size_t sizeApprox() const noexcept {
    const std::size_t w{m_write.load(std::memory_order_acquire)};
    const std::size_t r{m_read.load(std::memory_order_acquire)};
    return w - r;
  }

 private:
  static constexpr std::size_t kMask{Capacity - 1};

  alignas(kCacheLine) std::atomic<std::size_t> m_write{0};
  std::size_t m_cached_read{0};  // producer-private; shares the producer line

  alignas(kCacheLine) std::atomic<std::size_t> m_read{0};
  std::size_t m_cached_write{0};  // consumer-private; shares the consumer line

  alignas(kCacheLine) std::array<T, Capacity> m_slots{};

  // Keeps whatever the allocator puts after us off the last slot's line.
  char m_tail_pad[kCacheLine]{};
};

// The capacities this exchange instantiates the ring at live in
// net/core/Tuning.hpp. They are the application's sizes, not the queue's.
}  // namespace Exchange::Net
