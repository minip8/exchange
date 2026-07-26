#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "net/core/Tuning.hpp"
#include "types/OrderBookId.hpp"
#include "types/Symbol.hpp"

namespace Exchange::Net {
using Exchange::Types::OrderBookId;
using Exchange::Types::Symbol;

struct BookInfo {
  Symbol symbol{};
  OrderBookId id{0};
  // Wire prices are integers; the display value is price / 10^price_scale.
  // No floats ever go on the wire, in either protocol.
  uint32_t price_scale{kDefaultPriceScale};
  // The L2 depth published for this book. Book-wide rather than
  // per-subscriber on purpose: md_seq is per book, so every subscriber must
  // receive exactly the same message stream or their sequence numbers gap.
  uint32_t md_depth{kDefaultMdDepth};
};

/*
The gateway's side table of everything about a book that the ENGINE does not
know and must not learn.

That framing is the whole justification for this class existing next to
MatchingEngine's own book map, so it is worth being precise about what is here
and why none of it can move:

  price_scale, md_depth   Presentation and market-data policy. The engine
                          deals in integer prices and has no concept of a
                          feed; pushing either into it would drag protocol
                          concerns into the benchmarked core.

  insertion order         ListBooks must give a client the same ordering on
                          every reconnect. The engine keys its books in an
                          unordered_map, which cannot promise that.

  the 32-bit id check     Event::book_id is uint32_t, narrowed from
                          OrderBookId's uint64. This is the one place that
                          assumption is enforced.

There is deliberately NO symbol index here. There was one, and it was dead
weight in both directions: nothing ever called find(Symbol), and add()'s
duplicate-symbol check was unreachable because MatchingLoop::onCreateBook
calls MatchingEngine::addOrderBook first, which already rejects a duplicate
with EngineError::DuplicateSymbol before this class is ever reached. Symbol
resolution belongs to the engine and already lives there as
MatchingEngine::resolve — see the "symbols resolve, they do not route" rule.
*/
class BookDirectory {
 public:
  // Fails only on a book id too large for Event's 32-bit book_id field, which
  // a dense counter will never reach in this process — but the invariant is
  // worth enforcing where it is assumed. Duplicate symbols cannot reach here:
  // the engine has already rejected them.
  bool add(const BookInfo& info);

  // Borrowed-or-absent: the returned pointer is owned by this directory and
  // is invalidated by the next add(). See the convention note in
  // OrderStore.hpp.
  const BookInfo* find(OrderBookId id) const;

  std::span<const BookInfo> all() const noexcept { return m_books; }
  std::size_t size() const noexcept { return m_books.size(); }

 private:
  std::vector<BookInfo> m_books{};
  std::unordered_map<OrderBookId, std::size_t> m_by_id{};
};
}  // namespace Exchange::Net
