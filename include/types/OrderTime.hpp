#pragma once

#include <chrono>

struct OrderTime {
  std::chrono::milliseconds value{};

  explicit OrderTime(std::chrono::milliseconds v) : value(v) {}

  auto operator<=>(const OrderTime&) const = default;
};
