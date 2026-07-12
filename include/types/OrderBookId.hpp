#pragma once

#include <compare>
#include <cstdint>
#include <functional>

namespace Exchange::Types {
struct OrderBookId {
  using T = uint64_t;
  T value{};

  explicit OrderBookId(T v) : value(v) {}

  std::strong_ordering operator<=>(const OrderBookId&) const = default;
};
}  // namespace Exchange::Types

template <>
struct std::hash<Exchange::Types::OrderBookId> {
  size_t operator()(const Exchange::Types::OrderBookId& id) const noexcept {
    return std::hash<uint64_t>{}(id.value);
  }
};
