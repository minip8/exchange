#pragma once

#include <concepts>
#include <expected>
#include <functional>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "engine/Fill.hpp"
#include "engine/Order.hpp"
#include "engine/OrderBook.hpp"
#include "types/OrderBookId.hpp"
#include "types/OrderId.hpp"

namespace Exchange::Engine {
class MatchingEngine {
 public:
  MatchingEngine() noexcept = default;
  MatchingEngine(const MatchingEngine&) = delete;
  MatchingEngine& operator=(const MatchingEngine&) = delete;
  MatchingEngine(MatchingEngine&&) noexcept = default;
  MatchingEngine& operator=(MatchingEngine&&) noexcept = default;
  ~MatchingEngine() noexcept = default;

  std::expected<std::reference_wrapper<const OrderBook>, std::string_view>
  getOrderBook(const OrderId&) const;

  std::expected<std::reference_wrapper<const OrderBook>, std::string_view>
  getOrderBook(const OrderBookId&) const;

  std::expected<std::reference_wrapper<OrderBook>, std::string_view>
  getOrderBook(const OrderId&);

  std::expected<std::reference_wrapper<OrderBook>, std::string_view>
  getOrderBook(const OrderBookId&);

  std::expected<std::vector<Fill>, std::string_view> addOrder(
      const OrderBookId&, Order order);

  std::expected<Order, std::string_view> removeOrder(const OrderId& order_id);

  // OrderBookId addOrderBook();

 private:
  template <typename Self = MatchingEngine>
    requires std::same_as<std::remove_const_t<Self>, MatchingEngine>
  static auto getOrderBookImpl(Self& self, const OrderId& order_id)
      -> std::expected<std::reference_wrapper<std::conditional_t<
                           std::is_const_v<Self>, const OrderBook, OrderBook>>,
                       std::string_view> {
    auto order_book_id_it{self.m_order_id_to_order_book_id.find(order_id)};
    if (order_book_id_it == self.m_order_id_to_order_book_id.end()) {
      return std::unexpected("OrderId not found");
    }
    return self.getOrderBook(order_book_id_it->second);
  }

  template <typename Self = MatchingEngine>
  static auto getOrderBookImpl(Self& self, const OrderBookId& order_book_id)
      -> std::expected<std::reference_wrapper<std::conditional_t<
                           std::is_const_v<Self>, const OrderBook, OrderBook>>,
                       std::string_view> {
    auto order_book_it{self.m_order_book_id_to_order_book.find(order_book_id)};
    if (order_book_it == self.m_order_book_id_to_order_book.end()) {
      return std::unexpected("OrderBookId not found");
    }
    return order_book_it->second;
  }

 private:
  std::unordered_map<OrderId, OrderBookId> m_order_id_to_order_book_id;
  std::unordered_map<OrderBookId, OrderBook> m_order_book_id_to_order_book;
};
}  // namespace Exchange::Engine
