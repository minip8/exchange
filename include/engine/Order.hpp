#pragma once

#include "types/OrderId.hpp"
#include "types/OrderPrice.hpp"
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
  Order(Order&&) noexcept = default;
  Order& operator=(Order&&) noexcept = default;
  ~Order() noexcept = default;

  explicit Order(OrderId id, OrderPrice price, OrderTime time,
                 OrderQuantity quantity, OrderSide side)
      : m_id(id),
        m_price(price),
        m_time(time),
        m_quantity(quantity),
        m_side(side) {}

  OrderId id() const { return m_id; };
  OrderPrice price() const { return m_price; }
  OrderTime time() const { return m_time; };
  OrderQuantity quantity() const { return m_quantity; }
  OrderSide side() const { return m_side; }

 private:
  OrderId m_id;
  OrderPrice m_price;
  OrderTime m_time;
  OrderQuantity m_quantity;
  OrderSide m_side;
};

}  // namespace Exchange::Engine
