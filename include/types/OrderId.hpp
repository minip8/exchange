#pragma once

#include <compare>
#include <cstdint>

namespace Exchange::Types {
struct OrderId {
  uint64_t value{};

  explicit OrderId(uint64_t v) : value(v) {}

  std::strong_ordering operator<=>(const OrderId&) const = default;
};
}  // namespace Exchange::Types