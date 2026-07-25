#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

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
  uint32_t price_scale{2};
  // The L2 depth published for this book. Book-wide rather than
  // per-subscriber on purpose: md_seq is per book, so every subscriber must
  // receive exactly the same message stream or their sequence numbers gap.
  uint32_t md_depth{10};
};

/*
MatchingEngine has no way to enumerate its books — there is no accessor, and
adding one is not worth perturbing the benchmarked core. ListBooks is
therefore served from here, a gateway-owned directory maintained alongside
every successful CreateBook.

Insertion order is preserved so the listing a client receives is stable
across reconnects.
*/
class BookDirectory {
 public:
  // Fails only on a duplicate symbol or a book id too large for Event's
  // 32-bit book_id field (which a dense counter will never reach in this
  // process, but the invariant is worth enforcing where it is assumed).
  bool add(const BookInfo& info);

  const BookInfo* find(const Symbol& symbol) const;
  const BookInfo* find(OrderBookId id) const;

  std::span<const BookInfo> all() const noexcept { return m_books; }
  std::size_t size() const noexcept { return m_books.size(); }

 private:
  std::vector<BookInfo> m_books{};
  std::unordered_map<Symbol, std::size_t> m_by_symbol{};
  std::unordered_map<OrderBookId, std::size_t> m_by_id{};
};
}  // namespace Exchange::Net
