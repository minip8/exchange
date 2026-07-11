#include "engine/MatchingEngine.hpp"

#include <expected>
#include <functional>
#include <string_view>

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

std::expected<void, std::string_view> MatchingEngine::addOrder(
    const OrderBookId& order_book_id, Order order) {
  return getOrderBook(order_book_id)
      .and_then([&order](OrderBook& order_book)
                    -> std::expected<void, std::string_view> {
        order_book.addOrder(std::move(order));
        return {};
      });
}

std::expected<Order, std::string_view> MatchingEngine::removeOrder(
    const OrderId& order_id) {
  OrderBookId order_book_id{m_order_id_to_order_book_id[order_id]};
  OrderBook& order_book{m_order_book_id_to_order_book[order_book_id]};
  return order_book.removeOrder(order_id);
}
}  // namespace Exchange::Engine