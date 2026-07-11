#pragma once

#include <chrono>

namespace Exchange::Types {
struct OrderTime {
  using T = std::chrono::time_point<std::chrono::high_resolution_clock>;
  T value{};

  OrderTime(T v) : value(v) {}

  auto operator<=>(const OrderTime&) const = default;
};
}  // namespace Exchange::Types