#pragma once

#include <optional>

#include "engine/Order.hpp"
#include "types/OrderId.hpp"
namespace Engine {

class OrderBook {
 public:
  OrderBook() noexcept;
  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;
  OrderBook(OrderBook&&) noexcept;
  OrderBook& operator=(OrderBook&&) noexcept;
  ~OrderBook() noexcept;

  std::optional<Order> addOrder(Order&&);
  std::optional<Order> removeOrder(const OrderId);

 private:
};

}  // namespace Engine