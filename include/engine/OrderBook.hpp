#pragma once

#include <concepts>
#include <optional>
#include <vector>

#include "engine/Order.hpp"
#include "types/OrderBookId.hpp"
#include "types/OrderId.hpp"
#include "types/OrderPrice.hpp"

namespace Exchange::Engine {
using namespace Exchange::Types;
class OrderBook {
 public:
  OrderBook() noexcept;
  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;
  OrderBook(OrderBook&&) noexcept;
  OrderBook& operator=(OrderBook&&) noexcept;
  ~OrderBook() noexcept;

  OrderBook(OrderBookId id) : m_id(id) {}

  std::vector<Order> addOrder(Order&&) noexcept;
  std::optional<Order> removeOrder(const OrderId&);
  std::span<const Order> buys() const noexcept { return m_buys; }
  std::span<const Order> sells() const noexcept { return m_sells; }

 private:
  template <typename F>
    requires std::invocable<F, OrderPrice, OrderPrice>
  std::vector<Order> match(const std::vector<Order>& left_orders,
                           const std::vector<Order>& right_orders,
                           const Order& left_order, const Order& right_order,
                           F match_predicate);

 private:
  OrderBookId m_id;
  std::vector<Order> m_buys{};
  std::vector<Order> m_sells{};
};

}  // namespace Exchange::Engine