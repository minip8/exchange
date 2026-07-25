#pragma once

#include <compare>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>

namespace Exchange::Engine::Types {
struct Symbol {
  using T = std::string;
  std::string value;

  explicit Symbol(T value_) : value(std::move(value_)) {}

  std::strong_ordering operator<=>(const Symbol&) const = default;
};
}  // namespace Exchange::Engine::Types

using namespace Exchange::Engine::Types;

template <>
struct std::hash<Symbol> {
  size_t operator()(const Symbol& symbol) const noexcept {
    return std::hash<std::string>{}(symbol.value);
  }
};
