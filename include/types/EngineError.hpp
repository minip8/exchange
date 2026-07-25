#pragma once

#include <cstdint>

namespace Exchange::Types {
enum class EngineError : uint8_t {
  Success = 0,
  OrderNotFound,
  OrderBookNotFound,
  SymbolNotFound,
  DuplicateSymbol,
  SymbolTooLong,
};
}
