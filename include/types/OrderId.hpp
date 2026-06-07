#pragma once

#include <compare>
#include <cstdint>

struct OrderId {
  uint64_t value{};

  explicit OrderId(uint64_t v) : value(v) {}

  auto operator<=>(const OrderId&) const = default;
};
