#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>

#include "net/core/Side.hpp"
#include "types/OrderBookId.hpp"

namespace Exchange::Net {
using Exchange::Types::OrderBookId;

struct Position {
  int64_t net_quantity{};  // positive long, negative short
  int64_t avg_cost{};      // weighted-average entry, in price units
  int64_t realized_pnl{};  // in price x quantity units
  int64_t mark_price{};    // last mark applied
  int64_t unrealizedPnl() const noexcept {
    return net_quantity * (mark_price - avg_cost);
  }
};

struct TraderBook {
  uint32_t trader_id{};
  uint64_t book_id{};
  bool operator==(const TraderBook&) const noexcept = default;
};
}  // namespace Exchange::Net

template <>
struct std::hash<Exchange::Net::TraderBook> {
  size_t operator()(const Exchange::Net::TraderBook& key) const noexcept {
    return std::hash<uint64_t>{}(key.book_id * 0x9e3779b97f4a7c15ull +
                                 key.trader_id);
  }
};

namespace Exchange::Net {
/*
Position and P&L accounting, on the matching thread — the one place that sees
every fill of every book, so no cross-thread aggregation is needed.

Accounting is by trader, not by session: your browser tab and your algo
client, logged on with the same key, share one position. That falls out of
the ownership model for free.

Everything is integer. Prices on the wire are integers scaled by the book's
price_scale, and quantities are whole units, so P&L in "price x quantity"
units is exact. Introducing a float anywhere here would make two clients
disagree about their own money.
*/
class Positions {
 public:
  // Applies one fill leg from this trader's point of view and returns the
  // updated position.
  const Position& applyFill(uint32_t trader_id, OrderBookId book_id, Side side,
                            uint64_t price, uint64_t quantity);

  // Re-marks against the current mid. Returns false if the trader holds no
  // position in that book (nothing to re-mark).
  bool mark(uint32_t trader_id, OrderBookId book_id, int64_t mark_price);

  const Position* find(uint32_t trader_id, OrderBookId book_id) const;

 private:
  std::unordered_map<TraderBook, Position> m_positions{};
};
}  // namespace Exchange::Net
