#pragma once

#include <cstdint>

#include "types/OrderSide.hpp"

namespace Exchange::Net {
/*
The wire's side field.

`Types::OrderSide` is a scoped enum with no fixed underlying type, so it is
`int`-sized. Command and Event have to be exactly 64 bytes, and the binary
protocol has to be byte-stable, so neither can embed a 4-byte side.

The obvious alternative — giving OrderSide a `: uint8_t` base — would reach
into an engine header and change `Order`'s layout, which would move the
benchmark numbers the whole flash1 gate exists to protect. The engine is
supposed to barely change, so the conversion lives here instead. The encoding
(Buy = 0, Sell = 1) is also what the JSON codec and the GUI use.
*/
enum class Side : uint8_t {
  Buy = 0,
  Sell = 1,
};

constexpr Types::OrderSide toEngine(Side side) noexcept {
  return side == Side::Buy ? Types::OrderSide::Buy : Types::OrderSide::Sell;
}

constexpr Side fromEngine(Types::OrderSide side) noexcept {
  return side == Types::OrderSide::Buy ? Side::Buy : Side::Sell;
}

constexpr Side opposite(Side side) noexcept {
  return side == Side::Buy ? Side::Sell : Side::Buy;
}

static_assert(sizeof(Side) == 1);
static_assert(toEngine(fromEngine(Types::OrderSide::Sell)) ==
              Types::OrderSide::Sell);
}  // namespace Exchange::Net
