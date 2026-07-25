#include "net/gateway/MarketDataPublisher.hpp"

#include <algorithm>

#include "engine/PriceLevel.hpp"

namespace Exchange::Net {
using Exchange::Engine::PriceLevel;

void MarketDataPublisher::registerBook(OrderBookId book_id, uint32_t depth) {
  BookMd& md{m_books[book_id]};
  md.depth = depth == 0 ? 1 : depth;
}

bool MarketDataPublisher::subscribe(OrderBookId book_id, uint32_t session_id) {
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
                                      uint32_t session_id) {
  const auto it{m_books.find(book_id)};
  if (it == m_books.end()) return false;
  auto& subscribers{it->second.subscribers};
  const auto removed{std::ranges::remove(subscribers, session_id)};
  if (removed.empty()) return false;
  subscribers.erase(removed.begin(), removed.end());
  return true;
}

void MarketDataPublisher::dropSession(uint32_t session_id) {
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
  for (const uint32_t session_id : md.subscribers) {
    Event event{};
    event.session_id = session_id;
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
                                       OrderBookId book_id, uint32_t session_id,
                                       uint32_t trader_id, uint64_t ts_ns,
                                       std::vector<Event>& out) {
  const auto it{m_books.find(book_id)};
  if (it == m_books.end()) return;
  BookMd& md{it->second};

  readWindow(book.buys(), md.depth, m_scratch_buys);
  readWindow(book.sells(), md.depth, m_scratch_sells);

  const uint64_t level_count{m_scratch_buys.size() + m_scratch_sells.size()};

  auto make{[&](EventType type) {
    Event event{};
    event.session_id = session_id;
    event.trader_id = trader_id;
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

void MarketDataPublisher::publishDeltas(const OrderBook& book,
                                        OrderBookId book_id, uint64_t ts_ns,
                                        std::vector<Event>& out) {
  const auto it{m_books.find(book_id)};
  if (it == m_books.end()) return;
  BookMd& md{it->second};

  readWindow(book.buys(), md.depth, m_scratch_buys);
  readWindow(book.sells(), md.depth, m_scratch_sells);

  auto diff{[&](const std::vector<MdLevel>& current,
                std::vector<MdLevel>& published, Side side) {
    if (current == published) return;
    if (!md.subscribers.empty()) {
      // Anything that was visible and is no longer — either deleted outright
      // or pushed past the depth boundary by a better level — must be sent
      // as quantity 0, or the client's ladder keeps a level that is not
      // there. This is the case a per-dirty-price scheme silently misses.
      for (const MdLevel& was : published) {
        const auto still{
            std::ranges::find(current, was.price, &MdLevel::price)};
        if (still == current.end()) {
          emitLevel(md, book_id, side, was.price, 0, ts_ns, out);
        }
      }
      for (const MdLevel& now : current) {
        const auto before{
            std::ranges::find(published, now.price, &MdLevel::price)};
        if (before == published.end() || before->quantity != now.quantity) {
          emitLevel(md, book_id, side, now.price, now.quantity, ts_ns, out);
        }
      }
    }
    published = current;
  }};

  diff(m_scratch_buys, md.published_buys, Side::Buy);
  diff(m_scratch_sells, md.published_sells, Side::Sell);
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
  for (const uint32_t session_id : md.subscribers) {
    Event event{};
    event.session_id = session_id;
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

bool MarketDataPublisher::mark(const OrderBook& book, OrderBookId book_id,
                               int64_t& out) const {
  // front() is the best level: buys are sorted descending, sells ascending,
  // and empty levels are erased rather than left behind.
  const auto buys{book.buys()};
  const auto sells{book.sells()};
  if (!buys.empty() && !sells.empty()) {
    out = static_cast<int64_t>(
        (buys.front().price.value + sells.front().price.value) / 2);
    return true;
  }
  if (!buys.empty()) {
    out = static_cast<int64_t>(buys.front().price.value);
    return true;
  }
  if (!sells.empty()) {
    out = static_cast<int64_t>(sells.front().price.value);
    return true;
  }
  const auto it{m_books.find(book_id)};
  if (it != m_books.end() && it->second.has_traded) {
    out = static_cast<int64_t>(it->second.last_trade_price);
    return true;
  }
  return false;
}
}  // namespace Exchange::Net
