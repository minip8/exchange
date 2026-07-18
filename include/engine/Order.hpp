#pragma once

#include "types/OrderId.hpp"
#include "types/OrderPrice.hpp"
#include "types/OrderQuantity.hpp"
#include "types/OrderSide.hpp"
#include "types/OrderTime.hpp"

namespace Exchange::Engine {
using namespace Exchange::Types;
struct Order {
  Order() = delete;
  Order(const Order&) noexcept = default;
  Order& operator=(const Order&) noexcept = default;
  Order(Order&&) noexcept = default;
  Order& operator=(Order&&) noexcept = default;
  ~Order() noexcept = default;

  explicit Order(OrderPrice price_, OrderTime time_, OrderQuantity quantity_,
                 OrderSide side_) noexcept
      : id(instance_count),
        price(price_),
        time(time_),
        quantity(quantity_),
        side(side_) {
    ++instance_count;
  }

  /*
  Caller-supplied id: the caller owns id uniqueness. Does not touch
  instance_count, so do not mix with the auto-id constructor in one book.
  */
  explicit Order(OrderId id_, OrderPrice price_, OrderTime time_,
                 OrderQuantity quantity_, OrderSide side_) noexcept
      : id(id_), price(price_), time(time_), quantity(quantity_), side(side_) {}

 public:
  OrderId id;
  OrderPrice price;
  OrderTime time;
  OrderQuantity quantity;
  OrderSide side;

 private:
  static inline OrderId::T instance_count{};
};

/*
Implicitly copies into the function.
*/
inline Order newOrderWithQuantity(Order order, OrderQuantity new_quantity) {
  order.quantity = new_quantity;
  return order;
}
}  // namespace Exchange::Engine
