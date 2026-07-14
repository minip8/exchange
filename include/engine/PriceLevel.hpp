#pragma once

#include <vector>

#include "engine/Order.hpp"
#include "types/OrderPrice.hpp"

namespace Exchange::Engine {
using namespace Exchange::Types;
struct PriceLevel {
  OrderPrice price;
  std::vector<Order> orders;
};
}  // namespace Exchange::Engine