#pragma once

#include <compare>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>

namespace Exchange::Types {
struct Symbol {
  using T = std::string;
  T value;

  explicit Symbol(T value_) : value(std::move(value_)) {}

  std::strong_ordering operator<=>(const Symbol&) const = default;
};
}  // namespace Exchange::Types

template <>
struct std::hash<Exchange::Types::Symbol> {
  size_t operator()(const Exchange::Types::Symbol& symbol) const noexcept {
    return std::hash<std::string>{}(symbol.value);
  }
};
