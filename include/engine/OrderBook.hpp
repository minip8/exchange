#pragma once

#include <expected>
#include <functional>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "engine/Fill.hpp"
#include "engine/Order.hpp"
#include "engine/PriceLevel.hpp"
#include "types/OrderBookId.hpp"
#include "types/OrderId.hpp"
#include "types/OrderPrice.hpp"
#include "types/OrderSide.hpp"

namespace Exchange::Engine {
using namespace Exchange::Types;
class OrderBook {
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
  std::span<const PriceLevel> buys() const noexcept { return m_buy_levels; }
  std::span<const PriceLevel> sells() const noexcept { return m_sell_levels; }
  OrderBookId id() const noexcept { return m_id; }

 private:
  std::vector<Fill> match(std::vector<PriceLevel>& resting_levels,
                          Order& aggressing_order,
                          match_predicate_t match_predicate);

  std::vector<Fill> match(std::vector<Order>& resting_orders,
                          Order& aggressing_order,
                          match_predicate_t match_predicate);

  std::optional<Fill> match(Order& aggressing_order, Order& resting_order,
                            match_predicate_t match_predicate) const;

  void tryInsertRestingOrder(Order&&);

  template <OrderSide>
  std::vector<PriceLevel>::iterator tryInsertPriceLevel(const OrderPrice);

  template <OrderSide>
  std::vector<PriceLevel>::iterator priceLevelIterator(const OrderPrice) const;

  template <OrderSide>
  std::vector<PriceLevel>::iterator priceLevelIterator(const OrderPrice);

  template <OrderSide side, typename Self>
  auto priceLevelIteratorImpl(this Self& self, const OrderPrice order_price)
      -> std::conditional_t<std::is_const_v<Self>,
                            std::vector<PriceLevel>::const_iterator,
                            std::vector<PriceLevel>::iterator>;

 private:
  OrderBookId m_id;
  std::vector<PriceLevel> m_buy_levels{};
  std::vector<PriceLevel> m_sell_levels{};
  std::unordered_map<OrderId, std::pair<OrderSide, OrderPrice>>
      m_order_id_to_side_and_price{};

 private:
  static inline OrderBookId::T instance_count{};
};
template <OrderSide side, typename Self>
auto OrderBook::priceLevelIteratorImpl(this Self& self,
                                   const OrderPrice order_price)
    -> std::conditional_t<std::is_const_v<Self>,
                          std::vector<PriceLevel>::const_iterator,
                          std::vector<PriceLevel>::iterator> {
  if constexpr (side == Types::OrderSide::Buy) {
    return std::ranges::find_if(self.m_buy_levels,
                                [order_price](const PriceLevel& price_level) {
                                  return price_level.price <= order_price;
                                });
  } else if constexpr (side == Types::OrderSide::Sell) {
    return std::ranges::find_if(self.m_sell_levels,
                                [order_price](const PriceLevel& price_level) {
                                  return price_level.price >= order_price;
                                });
  }
}
}  // namespace Exchange::Engine
