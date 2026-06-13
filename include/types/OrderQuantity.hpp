#pragma once

#include <compare>
#include <cstdint>

struct OrderQuantity {
  uint64_t value{};

  explicit OrderQuantity(uint64_t v) : value(v) {}

  std::strong_ordering operator<=>(const OrderQuantity&) const = default;
};
