#pragma once

#include <algorithm>
#include <concepts>
#include <optional>
#include <vector>

#include "engine/Order.hpp"
#include "types/OrderBookId.hpp"
#include "types/OrderId.hpp"
#include "types/OrderQuantity.hpp"

namespace Exchange::Engine {
using namespace Exchange::Types;
class OrderBook {
 public:
  OrderBook() noexcept = delete;
  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;
  OrderBook(OrderBook&&) noexcept = default;
  OrderBook& operator=(OrderBook&&) noexcept = default;
  ~OrderBook() noexcept = default;

  OrderBook(OrderBookId id) : m_id(id) {}

  std::vector<Order> addOrder(Order) noexcept;
  std::optional<Order> removeOrder(const OrderId&);
  std::span<const Order> buys() const noexcept { return m_buys; }
  std::span<const Order> sells() const noexcept { return m_sells; }

 private:
  template <typename F>
    requires std::predicate<F, const Order&, const Order&>
  std::vector<Order> match(std::vector<Order>& left_orders,
                           std::vector<Order>& right_orders, Order& left_order,
                           F match_predicate);

 private:
  OrderBookId m_id;
  std::vector<Order> m_buys{};
  std::vector<Order> m_sells{};
};

template <typename F>
  requires std::predicate<F, const Order&, const Order&>
std::vector<Order> OrderBook::match(
    [[maybe_unused]] std::vector<Order>& left_orders,
    std::vector<Order>& right_orders, Order& left_order, F match_predicate) {
  std::vector<Order> filled_orders;
  OrderQuantity quantity_matched{0};
  long fully_filled_count{0};

  while (!right_orders.empty() && left_order.quantity() > OrderQuantity{0}) {
    Order& right_order = right_orders.front();
    if (!match_predicate(left_order, right_order)) break;
    OrderQuantity to_match{
        std::min(left_order.quantity(), right_order.quantity())};
    quantity_matched += to_match;
    if (to_match == right_order.quantity()) {
      filled_orders.emplace_back(std::move(right_order));
    }
    right_order.setQuantity(right_order.quantity() - to_match);

    filled_orders.emplace_back(right_order.id(), right_order.price(),
                               right_order.time(), to_match,
                               right_order.side());

    left_order.setQuantity(left_order.quantity() - to_match);
    fully_filled_count += (right_order.quantity() == OrderQuantity{0});
  }

  right_orders.erase(right_orders.begin(),
                     right_orders.begin() + fully_filled_count);
  return filled_orders;
}
}  // namespace Exchange::Engine
