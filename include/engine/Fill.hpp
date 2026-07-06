#pragma once

#include "types/OrderId.hpp"
#include "types/OrderPrice.hpp"
#include "types/OrderQuantity.hpp"
#include "types/OrderSide.hpp"
#include "types/OrderTime.hpp"
namespace Exchange::Engine {
using namespace Types;
struct Fill {
  OrderId resting_order_id;
  OrderId aggressor_order_id;
  OrderSide aggressor_side;
  OrderPrice price;
  OrderTime time;
  OrderQuantity quantity;
};
}  // namespace Exchange::Engine
