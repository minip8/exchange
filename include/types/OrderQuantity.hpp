#pragma once

#include <compare>
#include <cstdint>

namespace Exchange::Types {
struct OrderQuantity {
  using T = uint64_t;
  T value{};

  explicit OrderQuantity(T v) : value(v) {}

  std::strong_ordering operator<=>(const OrderQuantity&) const = default;

  // Quantity +/- Quantity
  OrderQuantity& operator+=(const OrderQuantity& rhs) noexcept {
    value += rhs.value;
    return *this;
  }
  OrderQuantity& operator-=(const OrderQuantity& rhs) noexcept {
    value -= rhs.value;
    return *this;
  }

  // Quantity +/- raw offset
  OrderQuantity& operator+=(T rhs) noexcept {
    value += rhs;
    return *this;
  }
  OrderQuantity& operator-=(T rhs) noexcept {
    value -= rhs;
    return *this;
  }

  // Scaling
  OrderQuantity& operator*=(T rhs) noexcept {
    value *= rhs;
    return *this;
  }
  OrderQuantity& operator/=(T rhs) noexcept {
    value /= rhs;
    return *this;
  }
};

inline OrderQuantity operator+(OrderQuantity lhs,
                               const OrderQuantity& rhs) noexcept {
  return lhs += rhs;
}
inline OrderQuantity operator-(OrderQuantity lhs,
                               const OrderQuantity& rhs) noexcept {
  return lhs -= rhs;
}
inline OrderQuantity operator*(OrderQuantity lhs,
                               OrderQuantity::T rhs) noexcept {
  return lhs *= rhs;
}
inline OrderQuantity operator/(OrderQuantity lhs,
                               OrderQuantity::T rhs) noexcept {
  return lhs /= rhs;
}
}  // namespace Exchange::Types