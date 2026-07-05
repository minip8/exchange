#pragma once

#include <unordered_map>

#include "engine/Order.hpp"
#include "engine/OrderBook.hpp"
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

  void processOrder(Order order) {
    OrderBook& order_book{m_id_to_book[order.id()]};
    order_book.addOrder(std::move(order));
  }

 private:
  std::unordered_map<OrderId, OrderBook> m_id_to_book;
};
}  // namespace Exchange::Engine
