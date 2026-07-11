#pragma once

#include <compare>
#include <cstdint>

namespace Exchange::Types {
struct OrderPrice {
  using T = uint64_t;
  T value{};

  OrderPrice(T v) : value(v) {}

  std::strong_ordering operator<=>(const OrderPrice&) const = default;
};
}  // namespace Exchange::Types