#include "engine/OrderBook.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "engine/Fill.hpp"
#include "engine/Order.hpp"
#include "engine/PriceLevel.hpp"
#include "types/EngineError.hpp"
#include "types/OrderId.hpp"
#include "types/OrderPrice.hpp"
#include "types/OrderQuantity.hpp"
#include "types/OrderSide.hpp"

namespace Exchange::Engine {
template <OrderSide side>
std::vector<PriceLevel>::iterator OrderBook::priceLevelIterator(
    const OrderPrice order_price) const {
  return priceLevelIteratorImpl<side>(order_price);
}
template <OrderSide side>
std::vector<PriceLevel>::iterator OrderBook::priceLevelIterator(
    const OrderPrice order_price) {
  return priceLevelIteratorImpl<side>(order_price);
}
template <>
std::vector<PriceLevel>::iterator
OrderBook::tryInsertPriceLevel<OrderSide::Buy>(const OrderPrice order_price) {
  auto level_it{priceLevelIterator<OrderSide::Buy>(order_price)};

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
  auto level_it{priceLevelIterator<OrderSide::Sell>(order_price)};

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
  uint64_t fully_filled_count{};
  for (PriceLevel& level : resting_levels) {
    auto level_fills{match(level.orders, aggressing_order, match_predicate)};
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
std::vector<Fill> OrderBook::match(std::vector<Order>& resting_orders,
                                   Order& aggressing_order,
                                   match_predicate_t match_predicate) {
  std::vector<Fill> fills{};
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
std::expected<Order, EngineError> OrderBook::removeOrder(
    const OrderId& order_id) {
  auto try_erase = [&order_id, this](
                       std::vector<PriceLevel>& levels,
                       std::vector<PriceLevel>::iterator& level_it)
      -> std::expected<Order, EngineError> {
    auto& orders{level_it->orders};
    auto order_it{std::ranges::find(orders, order_id, &Order::id)};
    if (order_it == orders.end()) {
      return std::unexpected(EngineError::OrderNotFound);
    }
    Order value{std::move(*order_it)};
    m_order_id_to_side_and_price.erase(order_id);
    orders.erase(order_it);
    if (level_it->orders.empty()) {
      levels.erase(level_it);
    }
    return value;
  };

  auto order_price_it{m_order_id_to_side_and_price.find(order_id)};
  if (order_price_it == m_order_id_to_side_and_price.end()) {
    return std::unexpected(EngineError::OrderNotFound);
  }

  const auto& [order_side, order_price]{order_price_it->second};

  if (order_side == Types::OrderSide::Buy) {
    auto buy_levels_it{priceLevelIterator<OrderSide::Buy>(order_price)};
    return try_erase(m_buy_levels, buy_levels_it);
  } else {
    auto sell_levels_it{priceLevelIterator<OrderSide::Sell>(order_price)};
    return try_erase(m_sell_levels, sell_levels_it);
  }
}
bool OrderBook::contains(const OrderId& order_id) const noexcept {
  return m_order_id_to_side_and_price.contains(order_id);
}
std::expected<std::vector<Fill>, EngineError> OrderBook::modifyOrder(
    const OrderId& order_id) {
  auto expected_order{removeOrder(order_id)};
  if (expected_order.has_value()) {
    return addOrder(std::move(expected_order.value()));
  } else {
    return std::unexpected(expected_order.error());
  }
}
}  // namespace Exchange::Engine
