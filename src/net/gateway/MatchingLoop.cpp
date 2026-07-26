#include "net/gateway/MatchingLoop.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <utility>

#include "engine/Fill.hpp"
#include "engine/Order.hpp"
#include "engine/OrderBook.hpp"
#include "types/OrderId.hpp"
#include "types/OrderPrice.hpp"
#include "types/OrderQuantity.hpp"
#include "types/OrderTime.hpp"

namespace Exchange::Net {
using Exchange::Engine::Fill;
using Exchange::Engine::Order;
using Exchange::Engine::OrderBook;
using Exchange::Types::OrderId;
using Exchange::Types::OrderPrice;
using Exchange::Types::OrderQuantity;
using Exchange::Types::OrderTime;

namespace {
/*
Mirrors bench/flash1/adapter.cpp's timeFromSeq. OrderTime is priority, not a
clock: the book only requires that successive addOrder calls carry
non-decreasing times, which a monotonic counter satisfies for free and
without a single clock read.
*/
OrderTime timeFromSeq(uint64_t seq) {
  using TimePoint = OrderTime::T;
  return OrderTime{TimePoint{TimePoint::duration{static_cast<int64_t>(seq)}}};
}

// Market orders are synthesized as marketable limits against OrderPrice's
// unsigned ordering. Note that flash1's sign-bit-flip encoding is NOT wanted
// here — that exists only because the harness deals in signed ticks.
constexpr uint64_t kMarketBuyPrice{std::numeric_limits<uint64_t>::max()};
constexpr uint64_t kMarketSellPrice{1};
}  // namespace

MatchingLoop::MatchingLoop(EventSink& sink, MatchingLoopConfig config)
    : m_sink(sink), m_config(config) {
  m_out.reserve(256);
  m_touched_positions.reserve(8);
}

OrderTime MatchingLoop::nextTime() { return timeFromSeq(++m_seq); }

Event MatchingLoop::base(const Command& command, EventType type) const {
  Event event{};
  event.session_id = command.session_id;
  event.trader_id = command.trader_id;
  event.type = type;
  event.side = command.side;
  return event;
}

namespace {
// Command carries raw wire ids; the gateway's own state is typed. These two
// are the boundary, and they are the only place the conversion should appear.
SessionId sessionOf(const Command& command) noexcept {
  return SessionId{command.session_id};
}
TraderId traderOf(const Command& command) noexcept {
  return TraderId{command.trader_id};
}
}  // namespace

void MatchingLoop::reject(const Command& command, RejectCode code) {
  Event event{base(command, EventType::Reject)};
  event.payload.ack = AckPayload{.order_id = command.order_id,
                                 .client_order_id = command.client_order_id,
                                 .orig_order_id = 0,
                                 .price = command.price,
                                 .quantity = command.quantity,
                                 // NotYourOrder must never reach a client;
                                 // see RejectCode.hpp.
                                 .reject_code = toWire(code),
                                 .pad = {}};
  m_out.push_back(event);
}

void MatchingLoop::emitToTrader(TraderId trader_id, const Event& prototype) {
  const auto it{m_trader_sessions.find(trader_id)};
  if (it == m_trader_sessions.end()) return;
  for (const SessionId session_id : it->second) {
    Event copy{prototype};
    copy.session_id = session_id.value;
    copy.trader_id = trader_id.value;
    m_out.push_back(copy);
  }
}

void MatchingLoop::flush(SessionId origin_session) {
  if (m_out.empty()) return;
  // The GUI coalesces rendering on this flag, so a burst of level updates
  // paints once rather than once per level.
  m_out.back().flags |= EventFlags::kEndOfBatch;

  /*
  Mark the last event addressed back to the commanding session, so its I/O
  thread can decrement an exact in-flight count. Searching backwards is
  right: a command's own ack comes first and its market-data consequences
  last, and it is the last one that means "finished".

  Every client-originated command produces at least one event for its own
  session — that is why MdAck exists — so this loop finding nothing means
  either origin_session is 0 or the command came from somewhere that is not
  counting credits. Both are fine; nothing leaks either way.
  */
  if (origin_session != kNoSession) {
    for (auto it{m_out.rbegin()}; it != m_out.rend(); ++it) {
      if (it->session_id == origin_session.value) {
        it->flags |= EventFlags::kCommandComplete;
        break;
      }
    }
  }

  m_sink.publish(m_out);
  m_out.clear();
}

void MatchingLoop::handle(const Command& command) {
#ifndef NDEBUG
  if (m_owner_thread == std::thread::id{}) {
    m_owner_thread = std::this_thread::get_id();
  }
  // Everything below constructs Orders and (for CreateBook) OrderBooks, both
  // of which touch non-atomic static counters in the engine. One thread, or
  // the engine's id space silently corrupts.
  assert(m_owner_thread == std::this_thread::get_id());
#endif

  switch (command.type) {
    case CommandType::SessionOpened:
      onSessionOpened(command);
      break;
    case CommandType::SessionClosed:
      onSessionClosed(command);
      break;
    case CommandType::CreateBook:
      onCreateBook(command);
      break;
    case CommandType::ListBooks:
      onListBooks(command);
      break;
    case CommandType::NewOrder:
      onNewOrder(command);
      break;
    case CommandType::Cancel:
      onCancel(command);
      break;
    case CommandType::Amend:
      onAmend(command);
      break;
    case CommandType::SubscribeMd:
      onSubscribeMd(command);
      break;
    case CommandType::UnsubscribeMd:
      onUnsubscribeMd(command);
      break;
    case CommandType::GetSnapshot:
      onGetSnapshot(command);
      break;
    case CommandType::None:
      reject(command, RejectCode::MalformedMessage);
      break;
  }
  // Every position this command disturbed, as one update each rather than one
  // per fill leg. Must precede flush(), or they would land in the next batch.
  emitPositions();

  // SessionClosed passes kNoSession: there is no session left to credit.
  flush(command.type == CommandType::SessionClosed ? kNoSession
                                                   : sessionOf(command));
}

void MatchingLoop::handleBatch(std::span<const Command> commands) {
  for (const Command& command : commands) handle(command);
}

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------

void MatchingLoop::onSessionOpened(const Command& command) {
  m_sessions.insert_or_assign(
      sessionOf(command),
      SessionInfo{
          .trader_id = traderOf(command),
          .cancel_on_disconnect =
              (command.flags & CommandFlags::kCancelOnDisconnect) != 0});
  auto& sessions{m_trader_sessions[traderOf(command)]};
  if (std::ranges::find(sessions, sessionOf(command)) == sessions.end()) {
    sessions.push_back(sessionOf(command));
  }

  Event ack{base(command, EventType::LogonAck)};
  ack.payload.ack = AckPayload{.order_id = 0,
                               .client_order_id = command.client_order_id,
                               .orig_order_id = 0,
                               .price = 0,
                               .quantity = m_directory.size(),
                               .reject_code = RejectCode::None,
                               .pad = {}};
  m_out.push_back(ack);

  // The book listing follows the logon ack in the same batch, so a client is
  // never in a state where it is logged on but does not know the
  // symbol -> book_id mapping it needs to send an order.
  onListBooks(command);
}

void MatchingLoop::onSessionClosed(const Command& command) {
  const auto session_it{m_sessions.find(sessionOf(command))};
  if (session_it == m_sessions.end()) return;
  const SessionInfo info{session_it->second};

  if (info.cancel_on_disconnect) {
    // Default behaviour is the other one: resting orders SURVIVE a
    // disconnect, which is what GTC means. Cancel-on-disconnect is opt-in at
    // logon and is the right default for an algo client, not for a browser.
    for (const OrderId id : m_orders.idsForSession(sessionOf(command))) {
      const auto meta{m_orders.erase(id)};
      if (!meta.has_value()) continue;
      const auto removed{m_engine.removeOrder(id)};
      if (removed.has_value()) republish(meta->book_id, command.recv_ts_ns);
    }
  }

  m_publisher.dropSession(sessionOf(command));
  m_sessions.erase(session_it);

  const auto trader_it{m_trader_sessions.find(info.trader_id)};
  if (trader_it != m_trader_sessions.end()) {
    auto& sessions{trader_it->second};
    const auto removed{std::ranges::remove(sessions, sessionOf(command))};
    sessions.erase(removed.begin(), removed.end());
    if (sessions.empty()) m_trader_sessions.erase(trader_it);
  }
}

// ---------------------------------------------------------------------------
// Book administration
// ---------------------------------------------------------------------------

void MatchingLoop::onCreateBook(const Command& command) {
  if (!m_config.allow_client_book_creation) {
    reject(command, RejectCode::AuthFailed);
    return;
  }
  // The Symbol in the command was length-validated by the codec via
  // Symbol::tryMake — the explicit string_view constructor writes past the
  // end on more than 8 characters, so it must never see untrusted input.
  // An all-zero symbol would still be nonsense, so refuse it here.
  if (command.symbol.view().empty()) {
    reject(command, RejectCode::InvalidSymbol);
    return;
  }

  // OrderBook's constructor bumps a non-atomic static counter, so this line
  // is the reason book creation is a matching-thread command rather than
  // something the I/O thread could do directly.
  const auto book_id{m_engine.addOrderBook(OrderBook{command.symbol})};

  Event ack{base(command, EventType::CreateBookAck)};
  if (!book_id.has_value()) {
    ack.payload.book = BookPayload{.symbol = command.symbol,
                                   .book_id = 0,
                                   .price_scale = 0,
                                   .index = 0,
                                   .count = 0,
                                   .reject_code = toRejectCode(book_id.error()),
                                   .pad = {}};
    m_out.push_back(ack);
    return;
  }

  const BookInfo info{.symbol = command.symbol,
                      .id = *book_id,
                      .price_scale = command.aux != 0
                                         ? command.aux
                                         : m_config.default_price_scale,
                      .md_depth = m_config.default_md_depth};
  if (!m_directory.add(info)) {
    // The engine accepted it but the directory did not, which can only mean
    // a book id too large for Event's 32-bit field. Nothing sane to do but
    // say so; the book is live but undiscoverable.
    reject(command, RejectCode::InternalError);
    return;
  }
  m_publisher.registerBook(info.id, info.md_depth);

  ack.book_id = static_cast<uint32_t>(info.id.value);
  ack.payload.book = BookPayload{.symbol = info.symbol,
                                 .book_id = info.id.value,
                                 .price_scale = info.price_scale,
                                 .index = 0,
                                 .count = 1,
                                 .reject_code = RejectCode::None,
                                 .pad = {}};
  m_out.push_back(ack);
}

void MatchingLoop::onListBooks(const Command& command) {
  uint32_t index{0};
  for (const BookInfo& info : m_directory.all()) {
    Event entry{base(command, EventType::BookEntry)};
    entry.book_id = static_cast<uint32_t>(info.id.value);
    entry.payload.book = BookPayload{.symbol = info.symbol,
                                     .book_id = info.id.value,
                                     .price_scale = info.price_scale,
                                     .index = index,
                                     .count = info.md_depth,
                                     .reject_code = RejectCode::None,
                                     .pad = {}};
    m_out.push_back(entry);
    ++index;
  }

  Event end{base(command, EventType::BookListEnd)};
  end.payload.book = BookPayload{.symbol = {},
                                 .book_id = 0,
                                 .price_scale = 0,
                                 .index = 0,
                                 .count = index,
                                 .reject_code = RejectCode::None,
                                 .pad = {}};
  m_out.push_back(end);
}

// ---------------------------------------------------------------------------
// Order entry
// ---------------------------------------------------------------------------

void MatchingLoop::onNewOrder(const Command& command) {
  if (command.quantity == 0) {
    // A zero-quantity order silently vanishes inside the engine
    // (tryInsertRestingOrder drops it and it matches nothing), so the client
    // would get an ack for an order that does not exist. Refuse it here.
    reject(command, RejectCode::InvalidQuantity);
    return;
  }

  const bool market{(command.flags & CommandFlags::kMarket) != 0};
  if (!market && command.price == 0) {
    // OrderPrice{0} rests forever: a buy at 0 never crosses, and a sell at 0
    // crosses everything. Neither is a thing a client meant to do.
    reject(command, RejectCode::InvalidPrice);
    return;
  }

  const BookInfo* info{m_directory.find(OrderBookId{command.book_id})};
  if (info == nullptr) {
    reject(command, RejectCode::UnknownBook);
    return;
  }

  const SessionCoid coid{sessionOf(command), command.client_order_id};
  if (command.client_order_id != 0 && m_orders.coidInUse(coid)) {
    reject(command, RejectCode::DuplicateClientOrderId);
    return;
  }

  // Market orders become marketable limits with IOC, so any residual is
  // pulled below rather than resting at an absurd price.
  const uint64_t price{
      market ? (command.side == Side::Buy ? kMarketBuyPrice : kMarketSellPrice)
             : command.price};
  const TimeInForce tif{market ? TimeInForce::Ioc : command.tif};

  const OrderId order_id{m_next_order_id++};
  auto fills{m_engine.addOrder(
      info->id,
      Order{order_id, OrderPrice{price}, nextTime(),
            OrderQuantity{command.quantity}, toEngine(command.side)})};
  if (!fills.has_value()) {
    reject(command, toRejectCode(fills.error()));
    return;
  }

  uint64_t filled{0};
  for (const Fill& fill : *fills) filled += fill.quantity.value;
  const uint64_t leaves{command.quantity - filled};

  Event ack{base(command, EventType::OrderAck)};
  ack.book_id = static_cast<uint32_t>(info->id.value);
  ack.payload.ack = AckPayload{.order_id = order_id.value,
                               .client_order_id = command.client_order_id,
                               .orig_order_id = 0,
                               .price = price,
                               .quantity = leaves,
                               .reject_code = RejectCode::None,
                               .pad = {}};
  m_out.push_back(ack);

  // Track the residual before reporting fills, so that a resting order this
  // aggressor later trades against can be resolved. The aggressor itself is
  // never in the store at match time — its order was not in the book yet —
  // which is exactly why applyFill takes the aggressor's identity as
  // arguments rather than looking it up.
  if (leaves > 0 && tif == TimeInForce::Gtc) {
    m_orders.insert(
        order_id,
        OrderMeta{.trader_id = traderOf(command),
                  .session_id = sessionOf(command),
                  .client_order_id = command.client_order_id,
                  .book_id = info->id,
                  .price = OrderPrice{price},
                  .side = command.side,
                  .original_quantity = OrderQuantity{command.quantity},
                  .leaves = OrderQuantity{leaves}});
  }

  uint64_t remaining{command.quantity};
  for (const Fill& fill : *fills) {
    remaining -= fill.quantity.value;
    applyFill(command, fill, info->id, traderOf(command),
              command.client_order_id, remaining);
  }

  if (leaves > 0 && tif == TimeInForce::Ioc) {
    // The engine has already rested the remainder; IOC means pull it.
    const auto removed{m_engine.removeOrder(order_id)};
    Event cancel{base(command, EventType::CancelAck)};
    cancel.book_id = static_cast<uint32_t>(info->id.value);
    cancel.flags |= EventFlags::kFinal;
    cancel.payload.ack = AckPayload{
        .order_id = order_id.value,
        .client_order_id = command.client_order_id,
        .orig_order_id = 0,
        .price = price,
        .quantity = removed.has_value() ? removed->quantity.value : leaves,
        .reject_code = RejectCode::None,
        .pad = {}};
    m_out.push_back(cancel);
  }

  republish(info->id, command.recv_ts_ns);
}

void MatchingLoop::applyFill(const Command& command, const Fill& fill,
                             OrderBookId book_id, TraderId aggressor_trader,
                             uint64_t aggressor_coid,
                             uint64_t aggressor_leaves) {
  const uint64_t exec_id{m_next_exec_id++};
  const uint64_t price{fill.price.value};
  const uint64_t quantity{fill.quantity.value};

  // --- aggressor side ---
  Event aggressor{};
  aggressor.book_id = static_cast<uint32_t>(book_id.value);
  aggressor.type = EventType::ExecReport;
  aggressor.side = fromEngine(fill.aggressor_side);
  aggressor.flags |= EventFlags::kAggressor;
  if (aggressor_leaves == 0) aggressor.flags |= EventFlags::kFinal;
  aggressor.payload.exec =
      ExecPayload{.exec_id = exec_id,
                  .order_id = fill.aggressor_order_id.value,
                  .client_order_id = aggressor_coid,
                  .price = price,
                  .quantity = quantity,
                  .leaves = aggressor_leaves};
  emitToTrader(aggressor_trader, aggressor);

  // --- resting side ---
  // Fill carries neither owner nor symbol, so this is the lookup that makes
  // the gateway, not the engine, the authority on who owns what.
  const auto result{
      m_orders.applyFill(fill.resting_order_id, OrderQuantity{quantity})};
  if (result.has_value()) {
    Event resting{};
    resting.book_id = static_cast<uint32_t>(book_id.value);
    resting.type = EventType::ExecReport;
    resting.side = result->meta.side;
    if (result->final) resting.flags |= EventFlags::kFinal;
    resting.payload.exec =
        ExecPayload{.exec_id = m_next_exec_id++,
                    .order_id = fill.resting_order_id.value,
                    .client_order_id = result->meta.client_order_id,
                    .price = price,
                    .quantity = quantity,
                    .leaves = result->leaves.value};
    emitToTrader(result->meta.trader_id, resting);
  }

  m_publisher.publishTrade(book_id, price, quantity,
                           fromEngine(fill.aggressor_side), command.recv_ts_ns,
                           m_out);

  /*
  Position ARITHMETIC is per leg — each one genuinely moves the running
  average and realized P&L. Position REPORTING is not: the touched pair is
  recorded here and emitPositions() sends one update per (trader, book) once
  the command is done. See the protocol note in MatchingLoop.hpp.
  */
  m_positions.applyFill(aggressor_trader, book_id,
                        fromEngine(fill.aggressor_side), price, quantity);
  touchPosition(aggressor_trader, book_id);
  if (result.has_value()) {
    m_positions.applyFill(result->meta.trader_id, book_id, result->meta.side,
                          price, quantity);
    touchPosition(result->meta.trader_id, book_id);
  }
}

void MatchingLoop::touchPosition(TraderId trader_id, OrderBookId book_id) {
  const TouchedPosition touched{.trader_id = trader_id, .book_id = book_id};
  if (std::ranges::find(m_touched_positions, touched) ==
      m_touched_positions.end()) {
    m_touched_positions.push_back(touched);
  }
}

void MatchingLoop::emitPositions() {
  if (m_touched_positions.empty()) return;

  for (const TouchedPosition& touched : m_touched_positions) {
    /*
    Marking happens here rather than per leg, and it loses nothing: addOrder
    completes the entire match before the first leg is reported, so the book
    is already in its final state and every per-leg mark would have read the
    same value. Doing it once removes an engine lookup and a mid computation
    per leg per trader.
    */
    const auto book{m_engine.getOrderBook(touched.book_id)};
    if (book.has_value()) {
      const auto mark{m_publisher.mark(book->get(), touched.book_id)};
      if (mark.has_value()) {
        m_positions.mark(touched.trader_id, touched.book_id, *mark);
      }
    }

    const Position* position{
        m_positions.find(touched.trader_id, touched.book_id)};
    if (position == nullptr) continue;

    Event event{};
    event.book_id = static_cast<uint32_t>(touched.book_id.value);
    event.type = EventType::PositionUpdate;
    event.payload.pos = PosPayload{.book_id = touched.book_id.value,
                                   .net_quantity = position->net_quantity,
                                   .avg_cost = position->avg_cost,
                                   .realized_pnl = position->realized_pnl,
                                   .unrealized_pnl = position->unrealizedPnl(),
                                   .mark_price = position->mark_price};
    emitToTrader(touched.trader_id, event);
  }

  m_touched_positions.clear();
}

void MatchingLoop::republish(OrderBookId book_id, uint64_t ts_ns) {
  if (!m_publisher.hasSubscribers(book_id)) return;
  const auto book{m_engine.getOrderBook(book_id)};
  if (!book.has_value()) return;
  m_publisher.publishDeltas(book->get(), book_id, ts_ns, m_out);
}

std::optional<MatchingLoop::ResolvedTarget> MatchingLoop::resolveTarget(
    const Command& command) {
  OrderId target{command.order_id};
  if (command.order_id == 0) {
    const auto resolved{m_orders.resolveCoid(
        SessionCoid{sessionOf(command), command.client_order_id})};
    if (!resolved.has_value()) {
      reject(command, RejectCode::UnknownOrder);
      return std::nullopt;
    }
    target = *resolved;
  }

  const OrderMeta* found{m_orders.find(target)};
  if (found == nullptr) {
    // Either genuinely unknown, or already fully filled — the engine cannot
    // tell those apart (a fully-filled aggressor is never indexed), but from
    // here both mean "there is nothing left to cancel", which is the correct
    // answer. Answering from the store also avoids ever reaching
    // OrderBook::removeOrder with an id it has no entry for.
    reject(command, RejectCode::UnknownOrder);
    return std::nullopt;
  }
  if (found->trader_id != traderOf(command)) {
    // Logged as NotYourOrder, sent as UnknownOrder: see RejectCode.hpp.
    reject(command, RejectCode::NotYourOrder);
    return std::nullopt;
  }

  return ResolvedTarget{.id = target, .meta = *found};
}

void MatchingLoop::onCancel(const Command& command) {
  const auto target{resolveTarget(command)};
  if (!target.has_value()) return;
  const OrderId id{target->id};
  const OrderMeta& meta{target->meta};

  const auto removed{m_engine.removeOrder(id)};
  if (!removed.has_value()) {
    // The store and the book disagree, which should be impossible — the two
    // are updated in lockstep. Drop the stale entry so it cannot rot.
    m_orders.erase(id);
    reject(command, RejectCode::UnknownOrder);
    return;
  }
  m_orders.erase(id);

  Event ack{base(command, EventType::CancelAck)};
  ack.book_id = static_cast<uint32_t>(meta.book_id.value);
  ack.side = meta.side;
  ack.flags |= EventFlags::kFinal;
  ack.payload.ack = AckPayload{.order_id = id.value,
                               .client_order_id = meta.client_order_id,
                               .orig_order_id = 0,
                               .price = meta.price.value,
                               .quantity = removed->quantity.value,
                               .reject_code = RejectCode::None,
                               .pad = {}};
  m_out.push_back(ack);

  republish(meta.book_id, command.recv_ts_ns);
}

/*
Amend is remove + re-add under a NEW order id.

OrderBook::modifyOrder is unusable for this: it takes no new price or
quantity and simply re-adds the identical order. So amend does what
adapter.cpp does — removeOrder, then addOrder with the explicit-id
constructor — stamped with the CURRENT sequence number, because reusing the
original time would violate the book's non-decreasing-time invariant.

A new id rather than the original: reusing an id after a partial fill makes
the exec report stream ambiguous (which of two fills at the same id belongs
to which version of the order?), and minting a new one is what FIX's
OrigClOrdID chain does anyway.

PRIORITY IS ALWAYS LOST, even when the amend only shrinks quantity, because
tryInsertRestingOrder push_backs onto the level regardless of time. That is a
protocol-visible property and is documented as such; Phase 7's
`reduceQuantity` would be what makes amend-down priority-preserving.
*/
void MatchingLoop::onAmend(const Command& command) {
  if (command.quantity == 0) {
    reject(command, RejectCode::InvalidQuantity);
    return;
  }
  if (command.price == 0) {
    reject(command, RejectCode::InvalidPrice);
    return;
  }

  const auto target{resolveTarget(command)};
  if (!target.has_value()) return;
  const OrderId id{target->id};
  const OrderMeta& meta{target->meta};

  const auto removed{m_engine.removeOrder(id)};
  if (!removed.has_value()) {
    m_orders.erase(id);
    reject(command, RejectCode::UnknownOrder);
    return;
  }
  m_orders.erase(id);

  const OrderId new_id{m_next_order_id++};
  auto fills{m_engine.addOrder(
      meta.book_id,
      Order{new_id, OrderPrice{command.price}, nextTime(),
            OrderQuantity{command.quantity}, toEngine(meta.side)})};
  if (!fills.has_value()) {
    reject(command, toRejectCode(fills.error()));
    republish(meta.book_id, command.recv_ts_ns);
    return;
  }

  uint64_t filled{0};
  for (const Fill& fill : *fills) filled += fill.quantity.value;
  const uint64_t leaves{command.quantity - filled};

  Event ack{base(command, EventType::AmendAck)};
  ack.book_id = static_cast<uint32_t>(meta.book_id.value);
  ack.side = meta.side;
  ack.payload.ack = AckPayload{.order_id = new_id.value,
                               .client_order_id = command.client_order_id,
                               .orig_order_id = id.value,
                               .price = command.price,
                               .quantity = leaves,
                               .reject_code = RejectCode::None,
                               .pad = {}};
  m_out.push_back(ack);

  if (leaves > 0) {
    m_orders.insert(
        new_id, OrderMeta{.trader_id = meta.trader_id,
                          .session_id = meta.session_id,
                          .client_order_id = command.client_order_id,
                          .book_id = meta.book_id,
                          .price = OrderPrice{command.price},
                          .side = meta.side,
                          .original_quantity = OrderQuantity{command.quantity},
                          .leaves = OrderQuantity{leaves}});
  }

  uint64_t remaining{command.quantity};
  for (const Fill& fill : *fills) {
    remaining -= fill.quantity.value;
    applyFill(command, fill, meta.book_id, meta.trader_id,
              command.client_order_id, remaining);
  }

  republish(meta.book_id, command.recv_ts_ns);
}

// ---------------------------------------------------------------------------
// Market data
// ---------------------------------------------------------------------------

void MatchingLoop::onSubscribeMd(const Command& command) {
  const BookInfo* info{m_directory.find(OrderBookId{command.book_id})};
  if (info == nullptr) {
    reject(command, RejectCode::UnknownBook);
    return;
  }
  m_publisher.subscribe(info->id, sessionOf(command));
  // Subscribing and snapshotting happen in the same handler, between two
  // commands, so there is no window in which a delta could slip past the
  // snapshot. See the sequencing note in MarketDataPublisher.hpp.
  m_publisher.emitSnapshot(*m_engine.getOrderBook(info->id), info->id,
                           sessionOf(command), traderOf(command),
                           command.recv_ts_ns, m_out);
}

void MatchingLoop::onUnsubscribeMd(const Command& command) {
  const BookInfo* info{m_directory.find(OrderBookId{command.book_id})};
  if (info == nullptr) {
    reject(command, RejectCode::UnknownBook);
    return;
  }
  m_publisher.unsubscribe(info->id, sessionOf(command));

  // Acked rather than silent, so the client knows the feed stopped on
  // purpose — and so this command, like every other, produces an event for
  // its own session. See EventFlags::kCommandComplete.
  Event ack{base(command, EventType::MdAck)};
  ack.book_id = static_cast<uint32_t>(info->id.value);
  ack.payload.md = MdPayload{.md_seq = m_publisher.sequence(info->id),
                             .price = 0,
                             .quantity = 0,
                             .ts_ns = command.recv_ts_ns,
                             .aggregate = 0,
                             .depth = info->md_depth,
                             .level_side = Side::Buy,
                             .pad = {}};
  m_out.push_back(ack);
}

void MatchingLoop::onGetSnapshot(const Command& command) {
  const BookInfo* info{m_directory.find(OrderBookId{command.book_id})};
  if (info == nullptr) {
    reject(command, RejectCode::UnknownBook);
    return;
  }
  m_publisher.emitSnapshot(*m_engine.getOrderBook(info->id), info->id,
                           sessionOf(command), traderOf(command),
                           command.recv_ts_ns, m_out);
}
}  // namespace Exchange::Net
