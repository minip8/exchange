#pragma once

#include <cstdint>
enum class EngineError : uint8_t {
  Success = 0,
  OrderNotFound,
  OrderBookNotFound,
};
