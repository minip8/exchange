#pragma once

#include "types/OrderId.hpp"
#include "types/OrderPrice.hpp"
#include "types/OrderQuantity.hpp"
#include "types/OrderSide.hpp"
#include "types/OrderTime.hpp"

namespace Exchange::Engine {
using namespace Exchange::Types;
struct Order {
  // Order() = delete;
  // Order(const Order&) = delete;
  // Order& operator=(const Order&) = delete;
  // Order(Order&&) noexcept = default;
  // Order& operator=(Order&&) noexcept = default;
  // ~Order() noexcept = default;

  // explicit Order(OrderId id, OrderPrice price, OrderTime time,
  //                OrderQuantity quantity, OrderSide side)
  //     : id(id), price(price), time(time), quantity(quantity), side(side) {}

  OrderId id;
  OrderPrice price;
  OrderTime time;
  OrderQuantity quantity;
  OrderSide side;
};

}  // namespace Exchange::Engine
