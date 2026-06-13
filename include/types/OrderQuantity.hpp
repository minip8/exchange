#pragma once

#include <compare>
#include <cstdint>

namespace Exchange::Types {
struct OrderQuantity {
  using T = uint64_t;
  T value{};

  explicit OrderQuantity(T v) : value(v) {}

  std::strong_ordering operator<=>(const OrderQuantity&) const = default;
};
}  // namespace Exchange::Types