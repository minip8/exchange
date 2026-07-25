#pragma once

#include <array>
#include <bit>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <string_view>
#include <type_traits>

#include "types/EngineError.hpp"

namespace Exchange::Types {
/*
A ticker held inline as fixed-size, zero-padded bytes rather than a
`std::string`: trivially copyable, never allocates, and hashes in a single
integer operation. The zero padding is what makes `operator==` and `std::hash`
agree for symbols shorter than `kMaxLength`, so every constructor must leave the
unused bytes zeroed — never name `value` in a mem-init-list.
*/
struct Symbol {
  static constexpr std::size_t kMaxLength{8};
  using T = std::array<char, kMaxLength>;
  T value{};

  Symbol() noexcept = default;

  /*
  Precondition: `sv.size() <= kMaxLength`. The caller owns the length; use
  `tryMake` for untrusted input.
  */
  explicit constexpr Symbol(std::string_view sv) {
    for (std::size_t i{0}; i < sv.size(); ++i) value[i] = sv[i];
  }

  static constexpr std::expected<Symbol, EngineError> tryMake(
      std::string_view sv) {
    if (sv.size() > kMaxLength) {
      return std::unexpected(EngineError::SymbolTooLong);
    }
    return Symbol{sv};
  }

  constexpr std::string_view view() const noexcept {
    std::size_t length{0};
    while (length < kMaxLength && value[length] != '\0') ++length;
    return std::string_view{value.data(), length};
  }

  std::strong_ordering operator<=>(const Symbol&) const = default;
};

// The invariants the hash specialization below relies on.
static_assert(sizeof(Symbol) == sizeof(uint64_t));
static_assert(std::is_trivially_copyable_v<Symbol>);
static_assert(std::is_nothrow_default_constructible_v<Symbol>);
static_assert(Symbol{"AB"} == Symbol{"AB"});
static_assert(Symbol{"AB"} < Symbol{"ABC"});
static_assert(Symbol{"12345678"}.view() == "12345678");
static_assert(Symbol::tryMake("NVDA").value().view() == "NVDA");
static_assert(!Symbol::tryMake("123456789").has_value());
}  // namespace Exchange::Types

template <>
struct std::hash<Exchange::Types::Symbol> {
  size_t operator()(const Exchange::Types::Symbol& symbol) const noexcept {
    return std::hash<uint64_t>{}(std::bit_cast<uint64_t>(symbol.value));
  }
};
