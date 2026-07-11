#include "engine/MatchingEngine.hpp"

#include <expected>
#include <functional>
#include <string_view>
#include <vector>

#include "engine/Fill.hpp"
#include "engine/Order.hpp"
#include "engine/OrderBook.hpp"
#include "types/OrderBookId.hpp"

namespace Exchange::Engine {
std::expected<std::reference_wrapper<const OrderBook>, std::string_view>
MatchingEngine::getOrderBook(const OrderId& order_id) const {
  return getOrderBookImpl(*this, order_id);
}

std::expected<std::reference_wrapper<const OrderBook>, std::string_view>
MatchingEngine::getOrderBook(const OrderBookId& order_book_id) const {
  return getOrderBookImpl(*this, order_book_id);
}

std::expected<std::reference_wrapper<OrderBook>, std::string_view>
MatchingEngine::getOrderBook(const OrderId& order_id) {
  return getOrderBookImpl(*this, order_id);
}

std::expected<std::reference_wrapper<OrderBook>, std::string_view>
MatchingEngine::getOrderBook(const OrderBookId& order_book_id) {
  return getOrderBookImpl(*this, order_book_id);
}

std::expected<std::vector<Fill>, std::string_view> MatchingEngine::addOrder(
    const OrderBookId& order_book_id, Order order) {
  return getOrderBook(order_book_id)
      .and_then([&order](OrderBook& order_book)
                    -> std::expected<std::vector<Fill>, std::string_view> {
        return order_book.addOrder(std::move(order));
      });
}

std::expected<Order, std::string_view> MatchingEngine::removeOrder(
    const OrderId& order_id) {
  return getOrderBook(order_id).and_then(
      [&order_id](
          OrderBook& order_book) -> std::expected<Order, std::string_view> {
        return order_book.removeOrder(order_id);
      });
}
}  // namespace Exchange::Engine