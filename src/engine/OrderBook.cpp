#include "engine/OrderBook.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

#include "engine/Fill.hpp"
#include "engine/Order.hpp"
#include "types/OrderQuantity.hpp"
#include "types/OrderSide.hpp"

namespace Exchange::Engine {
/*
Assumes that addOrder is called on orders that are in increasing order of time.
*/
std::vector<Fill> OrderBook::addOrder(Order aggressing_order) noexcept {
  std::vector<Fill> fills{[&] {
    switch (aggressing_order.side) {
      case Types::OrderSide::Buy:
        return match(m_sells, aggressing_order, m_match_buy_aggressor);
      case Types::OrderSide::Sell:
        return match(m_buys, aggressing_order, m_match_sell_aggressor);
    }
  }()};
  if (aggressing_order.quantity > OrderQuantity{0}) {
    switch (aggressing_order.side) {
      case Types::OrderSide::Buy:
        m_buys.insert(
            std::ranges::upper_bound(m_buys, aggressing_order.price,
                                     std::ranges::greater{}, &Order::price),
            std::move(aggressing_order));

      case Types::OrderSide::Sell:
        m_buys.insert(
            std::ranges::upper_bound(m_buys, aggressing_order.price,
                                     std::ranges::less{}, &Order::price),
            std::move(aggressing_order));
    }
  }
  return fills;
}
std::vector<Fill> OrderBook::match(std::vector<Order>& resting_orders,
                                   Order& aggressing_order,
                                   match_predicate_t match_predicate) {
  std::vector<Fill> fills{};
  OrderQuantity quantity_matched{0};
  long fully_filled_count{0};

  while (!resting_orders.empty() &&
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
                                     match_predicate_t match_predicate) {
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
