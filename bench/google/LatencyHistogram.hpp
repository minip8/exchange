#pragma once

/*
A fixed-memory latency histogram, for the one benchmark in this suite that
measures a *distribution* rather than a mean.

Why not a sample vector. `bench/net/net_workload_bench.cpp` keeps every sample
in a `std::vector<uint64_t>` and sorts it, which is fine there — the samples are
collected around a whole gateway call and the vector is filled outside anything
being timed. Here the recording site is inside the timed region of a 2M-message
replay, and a `push_back` there is wrong twice over: the amortised growth means
one unlucky sample absorbs an entire reallocation (a multi-ms spike that reads
as an engine stall, not as the instrument), and streaming 16 MB per op-kind
through the cache evicts the very book levels the benchmark is measuring.

So: HdrHistogram-style log-linear bucketing. 32 sub-buckets per octave, which
bounds the reported error at 2^-5 = 3.125%, in 3.4 KB that stays resident in
L1d. Recording is one `lzcnt`, two shifts and an increment.

Everything here is in *ticks*, never nanoseconds. The conversion factor is a
property of the timer (see TscTimer.hpp) and is applied once at report time, so
the hot path never touches a float.
*/

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace Exchange::Bench {

// 32 sub-buckets per power of two. Raising this to 6 halves the error and
// doubles the memory; nothing else in the file needs to change.
inline constexpr int kSubBucketBits{5};
inline constexpr std::size_t kSubBucketCount{std::size_t{1} << kSubBucketBits};

/*
The largest magnitude that gets its own buckets: values in [2^30, 2^31) land in
the last band, and anything at or above 2^31 ticks (~0.7 s at 3 GHz) is counted
in `overflow` instead. A latency that large is a machine event, not an engine
one, and giving it buckets would only widen the array.
*/
inline constexpr int kMaxMagnitude{30};

// Values below kSubBucketCount are stored exactly, one bucket each; above it,
// one band of kSubBucketCount buckets per magnitude from kSubBucketBits up.
inline constexpr std::size_t kBucketCount{
    kSubBucketCount +
    static_cast<std::size_t>(kMaxMagnitude - kSubBucketBits + 1) *
        kSubBucketCount};

// Bucket holding `ticks`, or kBucketCount for an overflowing value.
constexpr std::size_t bucketIndex(uint64_t ticks) noexcept {
  if (ticks < kSubBucketCount) return static_cast<std::size_t>(ticks);
  const int magnitude{63 - std::countl_zero(ticks)};  // 2^m <= ticks < 2^(m+1)
  if (magnitude > kMaxMagnitude) return kBucketCount;
  const auto band{static_cast<std::size_t>(magnitude - kSubBucketBits + 1)};
  const auto sub{static_cast<std::size_t>(
      (ticks >> (magnitude - kSubBucketBits)) & (kSubBucketCount - 1))};
  return (band << kSubBucketBits) + sub;
}

// Half-open [lo, hi) tick range covered by `index`.
constexpr std::pair<uint64_t, uint64_t> bucketRange(
    std::size_t index) noexcept {
  if (index < kSubBucketCount) return {index, index + 1};
  const int magnitude{static_cast<int>(index >> kSubBucketBits) - 1 +
                      kSubBucketBits};
  const int shift{magnitude - kSubBucketBits};
  const uint64_t lo{(kSubBucketCount | (index & (kSubBucketCount - 1)))
                    << shift};
  return {lo, lo + (uint64_t{1} << shift)};
}

/*
The linear region and the first exponential band must meet exactly, or a value
silently lands in two different buckets depending on which branch it took. Every
one of these has been wrong in some hand-rolled histogram somewhere.
*/
static_assert(bucketIndex(0) == 0);
static_assert(bucketIndex(31) == 31);
static_assert(bucketIndex(32) == 32, "linear region must abut the first band");
static_assert(bucketIndex(63) == 63);
static_assert(bucketIndex(64) == 64, "band boundary must be continuous");
static_assert(bucketIndex(65) == 64, "two ticks per bucket at magnitude 6");
static_assert(bucketIndex(127) == 95);
static_assert(bucketIndex(128) == 96);
static_assert(bucketIndex(uint64_t{1} << 30) == 832);
static_assert(bucketIndex((uint64_t{1} << 31) - 1) == kBucketCount - 1);
static_assert(bucketIndex(uint64_t{1} << 31) == kBucketCount, "overflow");
static_assert(bucketRange(32).first == 32 && bucketRange(32).second == 33);
static_assert(bucketRange(64).first == 64 && bucketRange(64).second == 66);
static_assert(bucketRange(kBucketCount - 1).second == uint64_t{1} << 31);

/*
Counts are uint32_t: a single bucket saturates at 4.29e9 samples, which at ~2M
messages per replay needs ~2000 repetitions to reach. `total` is 64-bit, so the
guard is a post-run check on it rather than a branch on the hot path.
*/
inline constexpr uint64_t kCountSaturation{4'000'000'000ull};

class LatencyHistogram {
 public:
  // The hot path. `ticks` must already be validated non-negative by the caller
  // — a backwards TSC delta is a machine event to be counted, not recorded.
  void record(uint64_t ticks) noexcept {
    const std::size_t index{bucketIndex(ticks)};
    if (index == kBucketCount) [[unlikely]] {
      ++m_overflow;
    } else {
      ++m_counts[index];
    }
    ++m_total;
    m_sum += ticks;
    m_min = std::min(m_min, ticks);
    m_max = std::max(m_max, ticks);
  }

  void merge(const LatencyHistogram& other) noexcept {
    for (std::size_t i{0}; i < kBucketCount; ++i)
      m_counts[i] += other.m_counts[i];
    m_overflow += other.m_overflow;
    m_total += other.m_total;
    m_sum += other.m_sum;
    m_min = std::min(m_min, other.m_min);
    m_max = std::max(m_max, other.m_max);
  }

  void reset() noexcept { *this = LatencyHistogram{}; }

  uint64_t total() const noexcept { return m_total; }
  uint64_t overflow() const noexcept { return m_overflow; }
  uint64_t minTicks() const noexcept { return m_total == 0 ? 0 : m_min; }
  uint64_t maxTicks() const noexcept { return m_max; }
  double meanTicks() const noexcept {
    return m_total == 0
               ? 0.0
               : static_cast<double>(m_sum) / static_cast<double>(m_total);
  }
  bool saturated() const noexcept { return m_total >= kCountSaturation; }
  uint64_t count(std::size_t index) const noexcept { return m_counts[index]; }

  /*
  Upper edge of the bucket containing the q-th quantile, so a reported value is
  always >= the true one and within +3.125%. Overflowing samples sit past the
  last bucket, so a quantile that falls in them reports `maxTicks()` — exact,
  since the extremes are tracked outside the buckets.
  */
  uint64_t quantileTicks(double q) const noexcept {
    if (m_total == 0) return 0;
    const auto target{static_cast<uint64_t>(q * static_cast<double>(m_total))};
    uint64_t seen{0};
    for (std::size_t i{0}; i < kBucketCount; ++i) {
      seen += m_counts[i];
      if (seen > target) return bucketRange(i).second;
    }
    return m_max;
  }

  // [first, last) indices of the occupied range, so the JSON writer emits the
  // ~200 buckets a real distribution occupies rather than all 864. Interior
  // zeros are kept: a gap between two modes is data.
  std::pair<std::size_t, std::size_t> occupiedRange() const noexcept {
    std::size_t first{0};
    while (first < kBucketCount && m_counts[first] == 0) ++first;
    if (first == kBucketCount) return {0, 0};
    std::size_t last{kBucketCount};
    while (last > first && m_counts[last - 1] == 0) --last;
    return {first, last};
  }

 private:
  std::array<uint32_t, kBucketCount> m_counts{};
  uint64_t m_total{0};
  uint64_t m_overflow{0};
  uint64_t m_sum{0};
  uint64_t m_min{UINT64_MAX};
  uint64_t m_max{0};
};

}  // namespace Exchange::Bench
