#pragma once

#include <expected>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "engine/Fill.hpp"
#include "engine/Order.hpp"
#include "types/OrderBookId.hpp"
#include "types/OrderId.hpp"

namespace Exchange::Engine {
using namespace Exchange::Types;
class OrderBook {
 public:
  //   OrderBook() noexcept = delete;
  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;
  OrderBook(OrderBook&&) noexcept = default;
  OrderBook& operator=(OrderBook&&) noexcept = default;
  ~OrderBook() noexcept = default;

  OrderBook() noexcept : m_id(instance_count) { ++instance_count; }

  std::vector<Fill> addOrder(Order&&) noexcept;
  std::expected<Order, std::string_view> removeOrder(const OrderId&);
  std::span<const Order> buys() const noexcept { return m_buys; }
  std::span<const Order> sells() const noexcept { return m_sells; }
  OrderBookId id() const noexcept { return m_id; }

 private:
  std::vector<Fill> match(
      std::vector<Order>& resting_orders, Order& aggressing_order,
      std::function_ref<bool(Order& aggressing_order, Order& resting_order)>
          match_predicate);

  std::optional<Fill> match(
      Order& aggressing_order, Order& resting_order,
      std::function_ref<bool(Order& aggressing_order, Order& resting_order)>
          match_predicate) const;

  void tryInsertResting(Order&&) noexcept;

 private:
  using match_predicate_t =
      std::function_ref<bool(Order& aggressing_order, Order& resting_order)>;

  match_predicate_t m_match_buy_aggressor{
      [](const Order& aggressing_order, const Order& resting_order) {
        return aggressing_order.price >= resting_order.price;
      }};
  match_predicate_t m_match_sell_aggressor{
      [](const Order& aggressing_order, const Order& resting_order) {
        return aggressing_order.price <= resting_order.price;
      }};

 private:
  OrderBookId m_id;
  std::vector<Order> m_buys{};
  std::vector<Order> m_sells{};

 private:
  static inline OrderBookId::T instance_count{};
};
}  // namespace Exchange::Engine
