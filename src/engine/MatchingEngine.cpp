#include "engine/MatchingEngine.hpp"

#include <expected>
#include <functional>
#include <vector>

#include "engine/Fill.hpp"
#include "engine/Order.hpp"
#include "engine/OrderBook.hpp"
#include "types/EngineError.hpp"
#include "types/OrderBookId.hpp"
#include "types/Symbol.hpp"

namespace Exchange::Engine {
std::expected<std::reference_wrapper<const OrderBook>, EngineError>
MatchingEngine::getOrderBook(const OrderId& order_id) const {
  return getOrderBookImpl(*this, order_id);
}

std::expected<std::reference_wrapper<const OrderBook>, EngineError>
MatchingEngine::getOrderBook(const OrderBookId& order_book_id) const {
  return getOrderBookImpl(*this, order_book_id);
}

std::expected<std::reference_wrapper<OrderBook>, EngineError>
MatchingEngine::getOrderBook(const OrderId& order_id) {
  return getOrderBookImpl(*this, order_id);
}

std::expected<std::reference_wrapper<OrderBook>, EngineError>
MatchingEngine::getOrderBook(const OrderBookId& order_book_id) {
  return getOrderBookImpl(*this, order_book_id);
}

std::expected<std::reference_wrapper<const OrderBook>, EngineError>
MatchingEngine::getOrderBook(const Symbol& symbol) const {
  return getOrderBookImpl(*this, symbol);
}

std::expected<std::reference_wrapper<OrderBook>, EngineError>
MatchingEngine::getOrderBook(const Symbol& symbol) {
  return getOrderBookImpl(*this, symbol);
}

std::expected<OrderBookId, EngineError> MatchingEngine::resolve(
    const Symbol& symbol) const {
  auto order_book_id_it{m_symbol_to_order_book_id.find(symbol)};
  if (order_book_id_it == m_symbol_to_order_book_id.end()) {
    return std::unexpected(EngineError::SymbolNotFound);
  }
  return order_book_id_it->second;
}

std::expected<std::vector<Fill>, EngineError> MatchingEngine::addOrder(
    const OrderBookId& order_book_id, Order&& order) {
  const OrderId order_id{order.id};
  return getOrderBook(order_book_id)
      .and_then([&](OrderBook& order_book)
                    -> std::expected<std::vector<Fill>, EngineError> {
        auto fills{order_book.addOrder(std::move(order))};
        // Only index the order if it actually rested (i.e. wasn't fully
        // filled on entry); otherwise there is nothing to look up later.
        if (order_book.contains(order_id)) {
          m_order_id_to_order_book_id.insert_or_assign(order_id, order_book_id);
        }
        return fills;
      });
}

std::expected<Order, EngineError> MatchingEngine::removeOrder(
    const OrderId& order_id) {
  return getOrderBook(order_id).and_then(
      [&](OrderBook& order_book) -> std::expected<Order, EngineError> {
        auto removed{order_book.removeOrder(order_id)};
        // The order no longer rests in any book, whether we removed it here or
        // it had already been consumed by a later aggressor. Drop the index
        // entry either way so it cannot go stale.
        m_order_id_to_order_book_id.erase(order_id);
        return removed;
      });
}

std::expected<std::vector<Fill>, EngineError> MatchingEngine::modifyOrder(
    const OrderId& order_id) {
  return getOrderBook(order_id).and_then(
      [&](OrderBook& order_book)
          -> std::expected<std::vector<Fill>, EngineError> {
        auto fills{order_book.modifyOrder(order_id)};
        // Re-adding may have fully filled the order; if it no longer rests,
        // the index entry must go. The order book is unchanged otherwise.
        if (fills.has_value() && !order_book.contains(order_id)) {
          m_order_id_to_order_book_id.erase(order_id);
        }
        return fills;
      });
}

std::expected<OrderBookId, EngineError> MatchingEngine::addOrderBook(
    OrderBook&& order_book) {
  // Read the identity out before the move, and check the symbol index first so
  // the rejected path leaves `order_book` un-moved-from.
  const Symbol symbol{order_book.symbol()};
  const OrderBookId order_book_id{order_book.id()};
  if (!m_symbol_to_order_book_id.try_emplace(symbol, order_book_id).second) {
    return std::unexpected(EngineError::DuplicateSymbol);
  }
  // `try_emplace`, not `insert_or_assign`: neither index may silently replace a
  // live book and orphan the order ids pointing at it.
  m_order_book_id_to_order_book.try_emplace(order_book_id,
                                            std::move(order_book));
  return order_book_id;
}
}  // namespace Exchange::Engine