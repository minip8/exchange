#include "engine/OrderBook.hpp"

#include <algorithm>
#include <expected>
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
std::vector<Fill> OrderBook::addOrder(Order&& aggressing_order) {
  std::vector<Fill> fills{[&] {
    switch (aggressing_order.side) {
      case Types::OrderSide::Buy:
        return match<Types::OrderSide::Buy>(aggressing_order);
      case Types::OrderSide::Sell:
        return match<Types::OrderSide::Sell>(aggressing_order);
      default:
        std::unreachable();
    }
  }()};
  tryInsertRestingOrder(std::move(aggressing_order));
  return fills;
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
