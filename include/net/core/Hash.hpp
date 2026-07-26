#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace Exchange::Net {
/*
2^64 / phi, the 64-bit fixed point of the golden ratio.

Two properties earn it its place, and both matter for the keys it is used on
here. It is ODD, so multiplication by it is a bijection on uint64_t — no two
inputs can ever collapse onto the same product, so the multiply itself throws
nothing away. And its bits are well distributed, so a low-entropy input (a
dense counter, which is what every id in this process is) comes out with
entropy spread across the whole word rather than concentrated in the bottom
few bits that std::unordered_map actually indexes buckets with.

This is the same constant Boost's hash_combine and the fibonacci-hashing
literature use, for the same reasons.
*/
inline constexpr uint64_t kGoldenRatio64{0x9e3779b97f4a7c15ull};

/*
Folds two independent 64-bit words into one hash.

`primary` is scrambled before `secondary` is added, so the two contribute at
different bit weights. That ordering is the whole point: both arguments are
typically dense small integers, and adding them unscrambled would make
(1000, 1) and (999, 2) collide. Pass the wider-ranging field as `primary`.
*/
constexpr std::size_t hashCombine(uint64_t primary,
                                  uint64_t secondary) noexcept {
  return std::hash<uint64_t>{}(primary * kGoldenRatio64 + secondary);
}
}  // namespace Exchange::Net
