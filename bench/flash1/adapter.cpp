/*
 * adapter.cpp — matching_engine_api.h backed by Exchange::Engine::OrderBook.
 *
 * The workload is single-instrument, so the adapter wraps one OrderBook
 * directly (no MatchingEngine routing). Harness order ids pass straight
 * through via Order's caller-supplied-id constructor; time priority is
 * synthesized from the message sequence number, which satisfies the book's
 * non-decreasing-time invariant deterministically.
 *
 * Report conventions mirror adapters/liquibook_adapter.cpp (the consensus
 * baseline): OrderAck before trades, always exactly one per new order;
 * Trade reports set only seq/price/quantity/maker/taker; CancelAck carries
 * the order's side and resting price with quantity 0 (IOC residual: the
 * unfilled remainder); rejects are zero except type/seq/order_id; ModifyAck
 * follows the reinsert's trades with the new price/quantity.
 *
 * The book matches synchronously on the calling thread, so engine_flush()
 * is a no-op.
 */
#include <cstdint>
#include <optional>
#include <vector>

#include "engine/Order.hpp"
#include "engine/OrderBook.hpp"
#include "matching_engine_api.h"
#include "types/OrderId.hpp"
#include "types/OrderPrice.hpp"
#include "types/OrderQuantity.hpp"
#include "types/OrderSide.hpp"
#include "types/OrderTime.hpp"
#include "types/Symbol.hpp"

#define ADAPTER_EXPORT __attribute__((visibility("default")))

#if defined(__aarch64__)
static inline void cpu_pause() { asm volatile("yield" ::: "memory"); }
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
static inline void cpu_pause() { _mm_pause(); }
#else
static inline void cpu_pause() {}
#endif

namespace {

using Exchange::Engine::Fill;
using Exchange::Engine::Order;
using Exchange::Engine::OrderBook;
namespace Types = Exchange::Types;

std::optional<OrderBook> g_book;
const me_transport_t* g_transport = nullptr;
void* g_sink = nullptr;

/*
Order-preserving bijection between the harness's signed price ticks and
OrderPrice's unsigned ordering: flipping the sign bit maps int64 order onto
uint64 order, so the book's price comparisons stay correct even for negative
ticks.
*/
inline Types::OrderPrice encodePrice(int64_t ticks) {
  return Types::OrderPrice{static_cast<uint64_t>(ticks) ^ (1ull << 63)};
}
inline int64_t decodePrice(Types::OrderPrice price) {
  return static_cast<int64_t>(price.value ^ (1ull << 63));
}
inline Types::OrderTime timeFromSeq(uint64_t seq) {
  using TimePoint = Types::OrderTime::T;
  return Types::OrderTime{
      TimePoint{TimePoint::duration{static_cast<int64_t>(seq)}}};
}
inline Types::OrderSide sideFrom(uint8_t side) {
  return side == 0 ? Types::OrderSide::Buy : Types::OrderSide::Sell;
}
inline uint8_t sideByte(Types::OrderSide side) {
  return side == Types::OrderSide::Buy ? 0 : 1;
}

void pushReport(const me_report_t& report) {
  while (!g_transport->push(g_sink, &report)) cpu_pause();
}

/* Non-trade reports: OrderAck / CancelAck / ModifyAck / rejects. */
void emitAck(uint8_t type, uint64_t seq, uint64_t order_id, uint8_t side,
             int64_t price_ticks, uint32_t quantity) {
  me_report_t r{};
  r.type = type;
  r.side = side;
  r.sequence_number = seq;
  r.order_id = order_id;
  r.price_ticks = price_ticks;
  r.quantity = quantity;
  pushReport(r);
}

/* One Trade per fill, in match order; seq is the aggressive order's. */
void emitTrades(const std::vector<Fill>& fills, uint64_t seq) {
  for (const Fill& fill : fills) {
    me_report_t r{};
    r.type = ME_TRADE;
    r.sequence_number = seq;
    r.price_ticks = decodePrice(fill.price);  // maker's resting price
    r.quantity = static_cast<uint32_t>(fill.quantity.value);
    r.maker_order_id = fill.resting_order_id.value;
    r.taker_order_id = fill.aggressor_order_id.value;
    pushReport(r);
  }
}

}  // namespace

extern "C" {

ADAPTER_EXPORT void engine_init(uint64_t /*seed*/,
                                const me_transport_t* transport,
                                void* report_sink) {
  g_transport = transport;
  g_sink = report_sink;
  // The harness is single-instrument and symbol-agnostic; the ticker is only
  // here because a book must name what it trades.
  g_book.emplace(Types::Symbol{"FLASH1"});
}

ADAPTER_EXPORT void engine_shutdown(void) { g_book.reset(); }

/* The book matches synchronously on the calling thread — nothing is pending. */
ADAPTER_EXPORT void engine_flush(void) {}

ADAPTER_EXPORT void engine_on_new_order(const new_order_t* o) {
  emitAck(ME_ORDER_ACK, o->sequence_number, o->order_id, o->side,
          o->price_ticks, o->quantity);

  std::vector<Fill> fills{g_book->addOrder(
      Order{Types::OrderId{o->order_id}, encodePrice(o->price_ticks),
            timeFromSeq(o->sequence_number), Types::OrderQuantity{o->quantity},
            sideFrom(o->side)})};
  emitTrades(fills, o->sequence_number);

  if (o->ioc) {
    uint64_t filled = 0;
    for (const Fill& fill : fills) filled += fill.quantity.value;
    if (filled < o->quantity) {
      /* addOrder rested the remainder; take it back out and ack the
       * residual cancellation. */
      (void)g_book->removeOrder(Types::OrderId{o->order_id});
      emitAck(ME_CANCEL_ACK, o->sequence_number, o->order_id, o->side,
              o->price_ticks, static_cast<uint32_t>(o->quantity - filled));
    }
  }
}

ADAPTER_EXPORT void engine_on_cancel(const cancel_t* c) {
  auto removed{g_book->removeOrder(Types::OrderId{c->order_id})};
  if (removed.has_value()) {
    emitAck(ME_CANCEL_ACK, c->sequence_number, c->order_id,
            sideByte(removed->side), decodePrice(removed->price), 0);
  } else {
    emitAck(ME_CANCEL_REJECT, c->sequence_number, c->order_id, 0, 0, 0);
  }
}

ADAPTER_EXPORT void engine_on_modify(const modify_t* m) {
  /* Cancel + reinsert at the new price/quantity (loses queue priority) — the
   * harness contract. */
  auto removed{g_book->removeOrder(Types::OrderId{m->order_id})};
  if (!removed.has_value()) {
    emitAck(ME_MODIFY_REJECT, m->sequence_number, m->order_id, 0, 0, 0);
    return;
  }
  std::vector<Fill> fills{g_book->addOrder(
      Order{Types::OrderId{m->order_id}, encodePrice(m->new_price_ticks),
            timeFromSeq(m->sequence_number),
            Types::OrderQuantity{m->new_quantity}, sideFrom(m->side)})};
  emitTrades(fills, m->sequence_number);
  emitAck(ME_MODIFY_ACK, m->sequence_number, m->order_id, m->side,
          m->new_price_ticks, m->new_quantity);
}

/* The book never erases emptied PriceLevels, so queries skip them. Levels are
 * sorted best-first; the first non-empty one is the best price. */
ADAPTER_EXPORT int64_t engine_query_best_bid(void) {
  for (const auto& level : g_book->buys()) {
    if (!level.orders.empty()) return decodePrice(level.price);
  }
  return INT64_MIN;
}

ADAPTER_EXPORT int64_t engine_query_best_ask(void) {
  for (const auto& level : g_book->sells()) {
    if (!level.orders.empty()) return decodePrice(level.price);
  }
  return INT64_MAX;
}

ADAPTER_EXPORT uint64_t engine_query_depth_at(int64_t price_ticks,
                                              uint8_t side) {
  const auto levels{side == 0 ? g_book->buys() : g_book->sells()};
  const Types::OrderPrice price{encodePrice(price_ticks)};
  for (const auto& level : levels) {
    if (level.price == price) {
      uint64_t total = 0;
      for (const Order& order : level.orders) total += order.quantity.value;
      return total;
    }
  }
  return 0;
}

/* Batch delivery: loop the per-message handlers in array order — identical
 * semantics to one-at-a-time delivery, minus the per-message cross-.so
 * dispatch overhead. */
ADAPTER_EXPORT void engine_on_batch(const me_msg_t* msgs, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) {
    const me_msg_t& m = msgs[i];
    if (m.type == 0) {
      engine_on_new_order(&m.no);
    } else if (m.type == 1) {
      engine_on_cancel(&m.c);
    } else {
      engine_on_modify(&m.md);
    }
  }
}

}  // extern "C"
