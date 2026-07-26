#include "net/gateway/Positions.hpp"

#include <algorithm>

namespace Exchange::Net {
namespace {
int64_t absValue(int64_t v) noexcept { return v < 0 ? -v : v; }

// The weighted-average numerator is quantity x price summed over a position,
// which can plausibly exceed int64 for a large book at a finely-scaled price.
// GCC's 128-bit integer removes the question; __extension__ keeps -Wpedantic
// enabled everywhere else rather than trading a real warning for this one.
__extension__ typedef __int128 Int128;
}  // namespace

const Position& Positions::applyFill(TraderId trader_id, OrderBookId book_id,
                                     Side side, uint64_t price,
                                     uint64_t quantity) {
  Position& position{m_positions[TraderBook{trader_id, book_id.value}]};

  const int64_t signed_price{static_cast<int64_t>(price)};
  const int64_t signed_quantity{static_cast<int64_t>(quantity)};
  const int64_t delta{side == Side::Buy ? signed_quantity : -signed_quantity};

  const bool opening{position.net_quantity == 0 ||
                     (position.net_quantity > 0) == (delta > 0)};

  if (opening) {
    // Weighted average of the existing book cost and the new lot. The
    // intermediate product is up to quantity x price, which for plausible
    // sizes stays well inside int64 — but __int128 costs nothing here and
    // removes the question entirely.
    const Int128 existing{static_cast<Int128>(absValue(position.net_quantity)) *
                          position.avg_cost};
    const Int128 added{static_cast<Int128>(signed_quantity) * signed_price};
    const int64_t total_quantity{absValue(position.net_quantity) +
                                 signed_quantity};
    position.avg_cost =
        total_quantity == 0
            ? 0
            : static_cast<int64_t>((existing + added) / total_quantity);
    position.net_quantity += delta;
  } else {
    // Reducing, closing, or flipping. Realize P&L on the portion that closes
    // against the existing average; anything beyond that opens a new
    // position at the fill price.
    const int64_t closing{
        std::min(absValue(position.net_quantity), signed_quantity)};
    const int64_t direction{position.net_quantity > 0 ? 1 : -1};
    position.realized_pnl +=
        direction * closing * (signed_price - position.avg_cost);
    position.net_quantity += delta;

    if (position.net_quantity == 0) {
      position.avg_cost = 0;
    } else if ((position.net_quantity > 0) != (direction > 0)) {
      // Flipped through flat: the residual is a fresh lot at this price.
      position.avg_cost = signed_price;
    }
  }

  // Absent a better mark, the last trade is the mark. MatchingLoop overrides
  // this with the mid when the book has two sides.
  position.mark_price = signed_price;
  return position;
}

bool Positions::mark(TraderId trader_id, OrderBookId book_id,
                     int64_t mark_price) {
  const auto it{m_positions.find(TraderBook{trader_id, book_id.value})};
  if (it == m_positions.end()) return false;
  it->second.mark_price = mark_price;
  return true;
}

const Position* Positions::find(TraderId trader_id, OrderBookId book_id) const {
  const auto it{m_positions.find(TraderBook{trader_id, book_id.value})};
  return it == m_positions.end() ? nullptr : &it->second;
}
}  // namespace Exchange::Net
