#pragma once

#include "types/OrderId.hpp"
#include "types/OrderQuantity.hpp"
#include "types/OrderSide.hpp"
#include "types/OrderTime.hpp"

namespace Exchange::Engine {
using namespace Exchange::Types;
class Order {
 public:
  Order() = delete;
  Order(const Order&) = delete;
  Order& operator=(const Order&) = delete;
  Order(Order&&) noexcept;
  Order& operator=(Order&&) noexcept;

  explicit Order(OrderId id, OrderTime time, OrderQuantity quantity,
                 OrderSide side)
      : m_id(id), m_time(time), m_quantity(quantity), m_side(side) {}

  OrderId id() const { return m_id; };

 private:
  OrderId m_id;
  OrderTime m_time;
  OrderQuantity m_quantity;
  OrderSide m_side;
};

}  // namespace Exchange::Engine
