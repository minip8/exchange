#pragma once

#include <compare>
#include <cstdint>

namespace Exchange::Types {
struct OrderBookId {
  using T = uint64_t;
  T value{};

  explicit OrderBookId(T v) : value(v) {}

  std::strong_ordering operator<=>(const OrderBookId&) const = default;
};
}  // namespace Exchange::Types