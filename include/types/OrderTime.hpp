#pragma once

#include <chrono>

namespace Exchange::Types {
struct OrderTime {
  std::chrono::milliseconds value{};

  explicit OrderTime(std::chrono::milliseconds v) : value(v) {}

  auto operator<=>(const OrderTime&) const = default;
};
}  // namespace Exchange::Types