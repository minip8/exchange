#pragma once

#include <concepts>
#include <expected>
#include <functional>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "engine/Fill.hpp"
#include "engine/Order.hpp"
#include "engine/OrderBook.hpp"
#include "types/EngineError.hpp"
#include "types/OrderBookId.hpp"
#include "types/OrderId.hpp"
#include "types/Symbol.hpp"

namespace Exchange::Engine {
class MatchingEngine {
 public:
  MatchingEngine() noexcept = default;
  MatchingEngine(const MatchingEngine&) = delete;
  MatchingEngine& operator=(const MatchingEngine&) = delete;
  MatchingEngine(MatchingEngine&&) noexcept = default;
  MatchingEngine& operator=(MatchingEngine&&) noexcept = default;
  ~MatchingEngine() noexcept = default;

  std::expected<std::reference_wrapper<const OrderBook>, EngineError>
  getOrderBook(const OrderId&) const;

  std::expected<std::reference_wrapper<const OrderBook>, EngineError>
  getOrderBook(const OrderBookId&) const;

  std::expected<std::reference_wrapper<OrderBook>, EngineError> getOrderBook(
      const OrderId&);

  std::expected<std::reference_wrapper<OrderBook>, EngineError> getOrderBook(
      const OrderBookId&);

  std::expected<std::reference_wrapper<const OrderBook>, EngineError>
  getOrderBook(const Symbol&) const;

  std::expected<std::reference_wrapper<OrderBook>, EngineError> getOrderBook(
      const Symbol&);

  // Resolves a ticker to the routing key. Order entry is deliberately
  // id-addressed only, so symbol hashing stays off the matching path.
  std::expected<OrderBookId, EngineError> resolve(const Symbol&) const;

  std::expected<std::vector<Fill>, EngineError> addOrder(const OrderBookId&,
                                                         Order&& order);

  std::expected<Order, EngineError> removeOrder(const OrderId& order_id);

  std::expected<std::vector<Fill>, EngineError> modifyOrder(const OrderId&);

  // Registers the book under its own symbol. Fails with `DuplicateSymbol`
  // rather than replacing an already-registered instrument.
  std::expected<OrderBookId, EngineError> addOrderBook(OrderBook&&);

 private:
  template <typename Self = MatchingEngine>
    requires std::same_as<std::remove_const_t<Self>, MatchingEngine>
  static auto getOrderBookImpl(Self& self, const OrderId& order_id)
      -> std::expected<std::reference_wrapper<std::conditional_t<
                           std::is_const_v<Self>, const OrderBook, OrderBook>>,
                       EngineError> {
    auto order_book_id_it{self.m_order_id_to_order_book_id.find(order_id)};
    if (order_book_id_it == self.m_order_id_to_order_book_id.end()) {
      return std::unexpected(EngineError::OrderNotFound);
    }
    return self.getOrderBook(order_book_id_it->second);
  }

  template <typename Self = MatchingEngine>
    requires std::same_as<std::remove_const_t<Self>, MatchingEngine>
  static auto getOrderBookImpl(Self& self, const Symbol& symbol)
      -> std::expected<std::reference_wrapper<std::conditional_t<
                           std::is_const_v<Self>, const OrderBook, OrderBook>>,
                       EngineError> {
    auto order_book_id_it{self.m_symbol_to_order_book_id.find(symbol)};
    if (order_book_id_it == self.m_symbol_to_order_book_id.end()) {
      return std::unexpected(EngineError::SymbolNotFound);
    }
    return self.getOrderBook(order_book_id_it->second);
  }

  template <typename Self = MatchingEngine>
  static auto getOrderBookImpl(Self& self, const OrderBookId& order_book_id)
      -> std::expected<std::reference_wrapper<std::conditional_t<
                           std::is_const_v<Self>, const OrderBook, OrderBook>>,
                       EngineError> {
    auto order_book_it{self.m_order_book_id_to_order_book.find(order_book_id)};
    if (order_book_it == self.m_order_book_id_to_order_book.end()) {
      return std::unexpected(EngineError::OrderBookNotFound);
    }
    return order_book_it->second;
  }

 private:
  std::unordered_map<OrderId, OrderBookId> m_order_id_to_order_book_id;
  std::unordered_map<Symbol, OrderBookId> m_symbol_to_order_book_id;
  std::unordered_map<OrderBookId, OrderBook> m_order_book_id_to_order_book;
};
}  // namespace Exchange::Engine
