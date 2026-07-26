#include "net/gateway/MarketDataPublisher.hpp"

#include <algorithm>

#include "engine/PriceLevel.hpp"

namespace Exchange::Net {
using Exchange::Engine::PriceLevel;

void MarketDataPublisher::registerBook(OrderBookId book_id, uint32_t depth) {
  BookMd& md{m_books[book_id]};
  md.depth = depth == 0 ? 1 : depth;
}

bool MarketDataPublisher::subscribe(OrderBookId book_id, SessionId session_id) {
  const auto it{m_books.find(book_id)};
  if (it == m_books.end()) return false;
  auto& subscribers{it->second.subscribers};
  if (std::ranges::find(subscribers, session_id) != subscribers.end()) {
    return false;
  }
  subscribers.push_back(session_id);
  return true;
}

bool MarketDataPublisher::unsubscribe(OrderBookId book_id,
                                      SessionId session_id) {
  const auto it{m_books.find(book_id)};
  if (it == m_books.end()) return false;
  auto& subscribers{it->second.subscribers};
  const auto removed{std::ranges::remove(subscribers, session_id)};
  if (removed.empty()) return false;
  subscribers.erase(removed.begin(), removed.end());
  return true;
}

void MarketDataPublisher::dropSession(SessionId session_id) {
  for (auto& [book_id, md] : m_books) {
    const auto removed{std::ranges::remove(md.subscribers, session_id)};
    md.subscribers.erase(removed.begin(), removed.end());
  }
}

bool MarketDataPublisher::hasSubscribers(OrderBookId book_id) const {
  const auto it{m_books.find(book_id)};
  return it != m_books.end() && !it->second.subscribers.empty();
}

uint64_t MarketDataPublisher::sequence(OrderBookId book_id) const {
  const auto it{m_books.find(book_id)};
  return it == m_books.end() ? 0 : it->second.md_seq;
}

/*
The level vectors are already sorted best-first (buys descending, sells
ascending) and empty levels are erased on the way out, so the best `depth`
levels are simply the first `depth` entries — no search, no skipping.

Summing each level's orders is the one O(k) step. k is 1-3 in practice.
Phase 7 could make it O(1) with a PriceLevel::total_quantity field, which
would also turn flash1's engine_query_depth_at from a loop into a lookup.
*/
void MarketDataPublisher::readWindow(std::span<const PriceLevel> levels,
                                     uint32_t depth,
                                     std::vector<MdLevel>& out) {
  out.clear();
  const std::size_t count{std::min<std::size_t>(depth, levels.size())};
  for (std::size_t i{0}; i < count; ++i) {
    uint64_t total{0};
    for (const auto& order : levels[i].orders) total += order.quantity.value;
    // A level whose orders sum to zero cannot exist — the engine erases a
    // level as soon as it empties — but skipping it costs nothing and keeps
    // "quantity 0 means deleted" unambiguous on the wire.
    if (total == 0) continue;
    out.push_back(MdLevel{.price = levels[i].price.value, .quantity = total});
  }
}

void MarketDataPublisher::emitLevel(BookMd& md, OrderBookId book_id, Side side,
                                    uint64_t price, uint64_t quantity,
                                    uint64_t ts_ns, std::vector<Event>& out) {
  ++md.md_seq;
  for (const SessionId session_id : md.subscribers) {
    Event event{};
    event.session_id = session_id.value;
    event.book_id = static_cast<uint32_t>(book_id.value);
    event.type = EventType::LevelUpdate;
    event.side = side;
    event.payload.md = MdPayload{.md_seq = md.md_seq,
                                 .price = price,
                                 .quantity = quantity,
                                 .ts_ns = ts_ns,
                                 .aggregate = 0,
                                 .depth = md.depth,
                                 .level_side = side,
                                 .pad = {}};
    out.push_back(event);
  }
}

void MarketDataPublisher::emitSnapshot(const OrderBook& book,
                                       OrderBookId book_id,
                                       SessionId session_id, TraderId trader_id,
                                       uint64_t ts_ns,
                                       std::vector<Event>& out) {
  const auto it{m_books.find(book_id)};
  if (it == m_books.end()) return;
  BookMd& md{it->second};

  readWindow(book.buys(), md.depth, m_scratch_buys);
  readWindow(book.sells(), md.depth, m_scratch_sells);

  const uint64_t level_count{m_scratch_buys.size() + m_scratch_sells.size()};

  auto make{[&](EventType type) {
    Event event{};
    event.session_id = session_id.value;
    event.trader_id = trader_id.value;
    event.book_id = static_cast<uint32_t>(book_id.value);
    event.type = type;
    return event;
  }};

  Event begin{make(EventType::SnapshotBegin)};
  begin.payload.md = MdPayload{.md_seq = md.md_seq,
                               .price = 0,
                               .quantity = 0,
                               .ts_ns = ts_ns,
                               .aggregate = level_count,
                               .depth = md.depth,
                               .level_side = Side::Buy,
                               .pad = {}};
  out.push_back(begin);

  auto emit_side{[&](const std::vector<MdLevel>& window, Side side) {
    for (const MdLevel& level : window) {
      Event event{make(EventType::LevelUpdate)};
      event.side = side;
      // Snapshot levels carry the CURRENT md_seq, not an incremented one:
      // a snapshot is a restatement of the state at that sequence, not a new
      // message in the delta stream. The next delta the client sees is
      // md_seq + 1, and that is exactly what its gap check expects.
      event.payload.md = MdPayload{.md_seq = md.md_seq,
                                   .price = level.price,
                                   .quantity = level.quantity,
                                   .ts_ns = ts_ns,
                                   .aggregate = 0,
                                   .depth = md.depth,
                                   .level_side = side,
                                   .pad = {}};
      out.push_back(event);
    }
  }};
  emit_side(m_scratch_buys, Side::Buy);
  emit_side(m_scratch_sells, Side::Sell);

  Event end{make(EventType::SnapshotEnd)};
  end.flags |= EventFlags::kEndOfBatch;
  end.payload.md = MdPayload{.md_seq = md.md_seq,
                             .price = 0,
                             .quantity = 0,
                             .ts_ns = ts_ns,
                             .aggregate = level_count,
                             .depth = md.depth,
                             .level_side = Side::Buy,
                             .pad = {}};
  out.push_back(end);

  // A snapshot is a full restatement, so it re-bases the published window.
  // This matters when a book spent time with no subscribers at all: deltas
  // are skipped then, and without this the next diff would replay every
  // change since the last subscriber left. Harmless (level updates are
  // idempotent) but pure noise.
  md.published_buys = m_scratch_buys;
  md.published_sells = m_scratch_sells;
}

/*
Two-pointer merge of the previously published window against the current one.

Both vectors arrive sorted best-first for their side — buys descending, sells
ascending — because readWindow copies them straight off level vectors the
engine already keeps in price-priority order. That is what makes a single
linear walk sufficient where the obvious formulation searches one window for
each entry of the other.

The three cases are exactly the three ways two sorted sequences can differ:

  same price          a quantity change, or nothing to say at all;
  current is better   a level that was not visible before — either brand new,
                      or promoted into view when something above it left;
  published is better a level that has left the client's view — deleted, or
                      pushed past the depth boundary by a better one.

The last of those is the case the whole diff-the-window design exists for: it
is not a "dirty price", nothing about that level changed, and a scheme that
looked up only touched prices would silently leave it on the client's ladder
forever.

Note this emits in price order, interleaving departures and arrivals, where
the previous formulation sent every departure before every arrival. Both are
valid L2 streams: each message carries its own md_seq, levels are
independent, and one price cannot both leave and arrive in a single diff
(equal prices merge into a single update above).
*/
void MarketDataPublisher::diffSide(BookMd& md, OrderBookId book_id, Side side,
                                   const std::vector<MdLevel>& current,
                                   std::vector<MdLevel>& published,
                                   uint64_t ts_ns, std::vector<Event>& out) {
  if (current == published) return;

  if (!md.subscribers.empty()) {
    // "Better" is side-relative: the best buy is the highest price, the best
    // sell the lowest. This is the same ordering the level vectors are in.
    const auto better{[side](uint64_t lhs, uint64_t rhs) noexcept {
      return side == Side::Buy ? lhs > rhs : lhs < rhs;
    }};

    std::size_t i{0};
    std::size_t j{0};
    while (i < current.size() && j < published.size()) {
      if (current[i].price == published[j].price) {
        if (current[i].quantity != published[j].quantity) {
          emitLevel(md, book_id, side, current[i].price, current[i].quantity,
                    ts_ns, out);
        }
        ++i;
        ++j;
      } else if (better(current[i].price, published[j].price)) {
        emitLevel(md, book_id, side, current[i].price, current[i].quantity,
                  ts_ns, out);
        ++i;
      } else {
        emitLevel(md, book_id, side, published[j].price, 0, ts_ns, out);
        ++j;
      }
    }
    for (; i < current.size(); ++i) {
      emitLevel(md, book_id, side, current[i].price, current[i].quantity, ts_ns,
                out);
    }
    for (; j < published.size(); ++j) {
      emitLevel(md, book_id, side, published[j].price, 0, ts_ns, out);
    }
  }

  published = current;
}

void MarketDataPublisher::publishDeltas(const OrderBook& book,
                                        OrderBookId book_id, uint64_t ts_ns,
                                        std::vector<Event>& out) {
  const auto it{m_books.find(book_id)};
  if (it == m_books.end()) return;
  BookMd& md{it->second};

  readWindow(book.buys(), md.depth, m_scratch_buys);
  readWindow(book.sells(), md.depth, m_scratch_sells);

  diffSide(md, book_id, Side::Buy, m_scratch_buys, md.published_buys, ts_ns,
           out);
  diffSide(md, book_id, Side::Sell, m_scratch_sells, md.published_sells, ts_ns,
           out);
}

void MarketDataPublisher::publishTrade(OrderBookId book_id, uint64_t price,
                                       uint64_t quantity, Side aggressor_side,
                                       uint64_t ts_ns,
                                       std::vector<Event>& out) {
  const auto it{m_books.find(book_id)};
  if (it == m_books.end()) return;
  BookMd& md{it->second};
  md.last_trade_price = price;
  md.has_traded = true;
  if (md.subscribers.empty()) return;

  ++md.md_seq;
  for (const SessionId session_id : md.subscribers) {
    Event event{};
    event.session_id = session_id.value;
    event.book_id = static_cast<uint32_t>(book_id.value);
    event.type = EventType::TradePrint;
    event.side = aggressor_side;
    event.payload.md = MdPayload{.md_seq = md.md_seq,
                                 .price = price,
                                 .quantity = quantity,
                                 .ts_ns = ts_ns,
                                 .aggregate = 0,
                                 .depth = md.depth,
                                 .level_side = aggressor_side,
                                 .pad = {}};
    out.push_back(event);
  }
}

std::optional<int64_t> MarketDataPublisher::mark(const OrderBook& book,
                                                 OrderBookId book_id) const {
  // front() is the best level: buys are sorted descending, sells ascending,
  // and empty levels are erased rather than left behind.
  const auto buys{book.buys()};
  const auto sells{book.sells()};
  if (!buys.empty() && !sells.empty()) {
    return static_cast<int64_t>(
        (buys.front().price.value + sells.front().price.value) / 2);
  }
  if (!buys.empty()) return static_cast<int64_t>(buys.front().price.value);
  if (!sells.empty()) return static_cast<int64_t>(sells.front().price.value);

  const auto it{m_books.find(book_id)};
  if (it != m_books.end() && it->second.has_traded) {
    return static_cast<int64_t>(it->second.last_trade_price);
  }
  return std::nullopt;
}
}  // namespace Exchange::Net
