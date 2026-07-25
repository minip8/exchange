#pragma once

#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "engine/Fill.hpp"
#include "engine/Order.hpp"
#include "engine/PriceLevel.hpp"
#include "types/EngineError.hpp"
#include "types/OrderBookId.hpp"
#include "types/OrderId.hpp"
#include "types/OrderPrice.hpp"
#include "types/OrderSide.hpp"
#include "types/Symbol.hpp"

namespace Exchange::Engine {
using namespace Exchange::Types;
class OrderBook {
 private:
  constexpr static inline auto m_match_buy_aggressor{
      [](const Order& aggressing_order, const Order& resting_order) {
        return aggressing_order.price >= resting_order.price;
      }};
  constexpr static inline auto m_match_sell_aggressor{
      [](const Order& aggressing_order, const Order& resting_order) {
        return aggressing_order.price <= resting_order.price;
      }};

 public:
  OrderBook() noexcept = delete;
  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;
  OrderBook(OrderBook&&) noexcept = default;
  OrderBook& operator=(OrderBook&&) noexcept = default;
  ~OrderBook() noexcept = default;

  // Every book trades exactly one named instrument. The id remains
  // process-wide monotonic and is not caller-supplied.
  explicit OrderBook(Symbol symbol) noexcept
      : m_id(instance_count), m_symbol(symbol) {
    ++instance_count;
  }

  std::vector<Fill> addOrder(Order&&);
  std::expected<Order, EngineError> removeOrder(const OrderId&);
  std::expected<std::vector<Fill>, EngineError> modifyOrder(const OrderId&);
  bool contains(const OrderId&) const noexcept;
  std::span<const PriceLevel> buys() const noexcept { return m_buy_levels; }
  std::span<const PriceLevel> sells() const noexcept { return m_sell_levels; }
  OrderBookId id() const noexcept { return m_id; }
  const Symbol& symbol() const noexcept { return m_symbol; }

 private:
  template <OrderSide>
  std::vector<Fill> match(Order& aggressing_order);

  template <OrderSide>
  std::vector<Fill> match(std::vector<Order>& resting_orders,
                          Order& aggressing_order);

  template <OrderSide>
  std::optional<Fill> match(Order& aggressing_order,
                            Order& resting_order) const;

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
  Symbol m_symbol;
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
template <OrderSide side>
std::vector<Fill> OrderBook::match(Order& aggressing_order) {
  std::vector<PriceLevel>& resting_levels{[this] -> std::vector<PriceLevel>& {
    if constexpr (side == Types::OrderSide::Buy) {
      return m_sell_levels;
    } else {
      return m_buy_levels;
    }
  }()};
  std::vector<Fill> fills{};
  uint64_t fully_filled_count{};
  for (PriceLevel& level : resting_levels) {
    auto level_fills{match<side>(level.orders, aggressing_order)};
    if (level_fills.empty()) break;
    fills.insert(fills.end(), level_fills.begin(), level_fills.end());
    fully_filled_count += static_cast<uint64_t>(level.orders.empty());
  }
  /*
  The `PriceLevel`s to be erased will always be the first `fully_filled_count`
  levels. It is impossible to be otherwise, because we greedily fill levels from
  left to right.
  */
  resting_levels.erase(
      resting_levels.begin(),
      resting_levels.begin() + static_cast<long>(fully_filled_count));
  return fills;
}
template <OrderSide side>
std::vector<Fill> OrderBook::match(std::vector<Order>& resting_orders,
                                   Order& aggressing_order) {
  std::vector<Fill> fills{};
  long fully_filled_count{0};

  while (static_cast<size_t>(fully_filled_count) < resting_orders.size() &&
         aggressing_order.quantity > OrderQuantity{0}) {
    Order& resting_order =
        resting_orders[static_cast<size_t>(fully_filled_count)];
    std::optional<Fill> fill{match<side>(aggressing_order, resting_order)};
    if (!fill.has_value()) break;
    fills.push_back(std::move(fill).value());
    fully_filled_count +=
        static_cast<long>(resting_order.quantity == OrderQuantity{0});
  }

  // The fully-filled resting orders are no longer in the book, so drop them
  // from the id index before erasing them from the level.
  for (long i{0}; i < fully_filled_count; ++i) {
    m_order_id_to_side_and_price.erase(
        resting_orders[static_cast<size_t>(i)].id);
  }

  resting_orders.erase(resting_orders.begin(),
                       resting_orders.begin() + fully_filled_count);

  return fills;
}
/*
Assumes positive order quantity.
Mutates the orders to reflect their updated quantities.
*/
template <OrderSide side>
std::optional<Fill> OrderBook::match(Order& aggressing_order,
                                     Order& resting_order) const {
  auto match_predicate{[this] {
    if constexpr (side == Types::OrderSide::Buy) {
      return m_match_buy_aggressor;
    } else {
      return m_match_sell_aggressor;
    }
  }()};
  if (match_predicate(aggressing_order, resting_order)) {
    OrderQuantity quantity_to_match{
        std::min(aggressing_order.quantity, resting_order.quantity)};

    aggressing_order.quantity -= quantity_to_match;
    resting_order.quantity -= quantity_to_match;

    return Fill{
        .resting_order_id = resting_order.id,
        .aggressor_order_id = aggressing_order.id,
        .aggressor_side = aggressing_order.side,
        .price = resting_order.price,
        .time = aggressing_order.time,
        .quantity = quantity_to_match,
    };
  }
  return {};
}
}  // namespace Exchange::Engine
