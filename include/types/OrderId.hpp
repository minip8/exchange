#pragma once

#include <compare>
#include <cstdint>

namespace Exchange::Types {
struct OrderId {
  using T = uint64_t;
  T value{};

  explicit OrderId(T v) : value(v) {}

  std::strong_ordering operator<=>(const OrderId&) const = default;
};
}  // namespace Exchange::Types