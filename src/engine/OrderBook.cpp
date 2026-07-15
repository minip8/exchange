#include "engine/OrderBook.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/Fill.hpp"
#include "engine/Order.hpp"
#include "engine/PriceLevel.hpp"
#include "types/OrderPrice.hpp"
#include "types/OrderQuantity.hpp"
#include "types/OrderSide.hpp"

namespace Exchange::Engine {
template <>
std::vector<PriceLevel>::iterator
OrderBook::tryInsertPriceLevel<OrderSide::Buy>(const OrderPrice order_price) {
  auto level_it{std::ranges::lower_bound(
      m_buy_levels, order_price, std::ranges::less{}, &PriceLevel::price)};

  if (level_it == m_buy_levels.end() || level_it->price != order_price) {
    return m_buy_levels.insert(level_it,
                               PriceLevel{.price = order_price, .orders = {}});
  } else {
    return level_it;
  }
}
template <>
std::vector<PriceLevel>::iterator
OrderBook::tryInsertPriceLevel<OrderSide::Sell>(const OrderPrice order_price) {
  auto level_it{std::ranges::lower_bound(
      m_sell_levels, order_price, std::ranges::greater{}, &PriceLevel::price)};

  if (level_it == m_sell_levels.end() || level_it->price != order_price) {
    return m_sell_levels.insert(level_it,
                                PriceLevel{.price = order_price, .orders = {}});
  } else {
    return level_it;
  }
}
/*
Assumes that addOrder is called on orders that are in increasing order of time.
*/
std::vector<Fill> OrderBook::addOrder(Order&& aggressing_order) noexcept {
  std::vector<Fill> fills{[&] {
    switch (aggressing_order.side) {
      case Types::OrderSide::Buy:
        return match(m_sell_levels, aggressing_order, m_match_buy_aggressor);
      case Types::OrderSide::Sell:
        return match(m_buy_levels, aggressing_order, m_match_sell_aggressor);
      default:
        std::unreachable();
    }
  }()};
  tryInsertRestingOrder(std::move(aggressing_order));
  return fills;
}
std::vector<Fill> OrderBook::match(std::vector<PriceLevel>& resting_levels,
                                   Order& aggressing_order,
                                   match_predicate_t match_predicate) {
  std::vector<Fill> fills{};
  for (PriceLevel& level : resting_levels) {
    if (level.orders.empty()) continue;
    auto level_fills{match(level.orders, aggressing_order, match_predicate)};
    if (level_fills.empty()) break;
    fills.insert(fills.end(), level_fills.begin(), level_fills.end());
  }
  return fills;
}
std::vector<Fill> OrderBook::match(std::vector<Order>& resting_orders,
                                   Order& aggressing_order,
                                   match_predicate_t match_predicate) {
  std::vector<Fill> fills{};
  OrderQuantity quantity_matched{0};
  long fully_filled_count{0};

  while (static_cast<size_t>(fully_filled_count) < resting_orders.size() &&
         aggressing_order.quantity > OrderQuantity{0}) {
    Order& resting_order =
        resting_orders[static_cast<size_t>(fully_filled_count)];
    std::optional<Fill> fill{
        match(aggressing_order, resting_order, match_predicate)};
    if (!fill.has_value()) break;
    fills.push_back(std::move(fill).value());
    fully_filled_count +=
        static_cast<long>(resting_order.quantity == OrderQuantity{0});
  }

  resting_orders.erase(resting_orders.begin(),
                       resting_orders.begin() + fully_filled_count);

  return fills;
}
/*
Assumes positive order quantity.
Mutates the orders to reflect their updated quantities.
*/
std::optional<Fill> OrderBook::match(Order& aggressing_order,
                                     Order& resting_order,
                                     match_predicate_t match_predicate) const {
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
void OrderBook::tryInsertRestingOrder(Order&& order) {
  if (order.quantity > OrderQuantity{0}) {
    m_order_id_to_side_and_price.insert_or_assign(
        order.id, std::make_pair(order.side, order.price));
    switch (order.side) {
      case Types::OrderSide::Buy: {
        auto level_it{tryInsertPriceLevel<Types::OrderSide::Buy>(order.price)};
        auto& level_orders{level_it->orders};
        level_orders.push_back(std::move(order));
      } break;

      case Types::OrderSide::Sell: {
        auto level_it{tryInsertPriceLevel<Types::OrderSide::Sell>(order.price)};
        auto& level_orders{level_it->orders};
        level_orders.push_back(std::move(order));
      } break;
    }
  }
}
std::expected<Order, std::string_view> OrderBook::removeOrder(
    const OrderId& order_id) {
  auto try_erase = [&order_id, this](std::vector<Order>& orders)
      -> std::expected<Order, std::string_view> {
    auto it{std::ranges::find(orders, order_id, &Order::id)};
    if (it == orders.end()) {
      return std::unexpected("Order not found");
    }
    Order value{std::move(*it)};
    orders.erase(it);
    m_order_id_to_side_and_price.erase(order_id);
    return value;
  };

  auto order_price_it{m_order_id_to_side_and_price.find(order_id)};
  if (order_price_it == m_order_id_to_side_and_price.end()) {
    return std::unexpected("Order not found");
  }

  const auto& [order_side, order_price]{order_price_it->second};

  if (order_side == Types::OrderSide::Buy) {
    auto& buy_orders{std::ranges::find_if(
                         m_buy_levels,
                         [order_price](OrderPrice element) {
                           return element <= order_price;
                         },
                         &PriceLevel::price)
                         ->orders};
    return try_erase(buy_orders);
  } else {
    auto& sell_orders{std::ranges::find_if(
                          m_sell_levels,
                          [order_price](OrderPrice element) {
                            return element >= order_price;
                          },
                          &PriceLevel::price)
                          ->orders};
    return try_erase(sell_orders);
  }
}
}  // namespace Exchange::Engine
