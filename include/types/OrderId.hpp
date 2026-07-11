#pragma once

#include <compare>
#include <cstdint>
#include <functional>

namespace Exchange::Types {
struct OrderId {
  using T = uint64_t;
  T value{};

  OrderId(T v) : value(v) {}

  std::strong_ordering operator<=>(const OrderId&) const = default;
};
}  // namespace Exchange::Types

template <>
struct std::hash<Exchange::Types::OrderId> {
  size_t operator()(const Exchange::Types::OrderId& id) const noexcept {
    return std::hash<uint64_t>{}(id.value);
  }
};
