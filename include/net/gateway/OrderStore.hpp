#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include "net/core/Side.hpp"
#include "types/OrderBookId.hpp"
#include "types/OrderId.hpp"
#include "types/OrderPrice.hpp"
#include "types/OrderQuantity.hpp"

namespace Exchange::Net {
using Exchange::Types::OrderBookId;
using Exchange::Types::OrderId;
using Exchange::Types::OrderPrice;
using Exchange::Types::OrderQuantity;

struct OrderMeta {
  uint32_t trader_id{};
  uint32_t session_id{};
  uint64_t client_order_id{};
  OrderBookId book_id{0};
  OrderPrice price{0};
  Side side{Side::Buy};
  OrderQuantity original_quantity{0};
  // The engine consumes the Order and returns only fills, so nobody else in
  // the process knows how much of an order is left. This field is what makes
  // the gateway the authoritative order-state store the engine lacks:
  // leaves = original - sum(fill.quantity).
  OrderQuantity leaves{0};
};

// Client order ids are unique per session, not globally, so the key is the
// pair. Cancel-by-coid resolves through this.
struct SessionCoid {
  uint32_t session_id{};
  uint64_t client_order_id{};
  bool operator==(const SessionCoid&) const noexcept = default;
};
}  // namespace Exchange::Net

template <>
struct std::hash<Exchange::Net::SessionCoid> {
  size_t operator()(const Exchange::Net::SessionCoid& key) const noexcept {
    // Two independent 64-bit words folded with the usual odd multiplier.
    // Client order ids are typically dense per session, so mixing the
    // session in at a different bit weight keeps buckets spread.
    const uint64_t a{key.client_order_id};
    const uint64_t b{static_cast<uint64_t>(key.session_id)};
    return std::hash<uint64_t>{}(a * 0x9e3779b97f4a7c15ull + b);
  }
};

namespace Exchange::Net {
/*
Order ownership and residual state, all of it on the matching thread.

That placement is non-negotiable: the ownership check has to happen *before*
removeOrder, and there is no way to un-cancel. It also lets the gateway
answer "unknown order" itself without touching the engine, which sidesteps
the ambiguity in EngineError::OrderNotFound (genuinely unknown vs. already
fully filled) and a latent end() dereference in OrderBook::removeOrder.

Lifecycle mirrors the engine exactly:
  - insert only if leaves > 0, matching "only index if it rested";
  - decrement on each fill against resting_order_id;
  - erase at 0, precisely when the engine drops it.

The aggressor's owner is already carried in the Command and can never be in
the store at match time, since its order is not in the book yet.
*/
class OrderStore {
 public:
  // Returns nullptr if `leaves` is 0 — nothing rested, so nothing is stored.
  const OrderMeta* insert(OrderId id, const OrderMeta& meta);

  const OrderMeta* find(OrderId id) const;

  // Applies a fill leg. Returns the new `leaves`, and erases the entry when
  // it reaches 0. Returns nullopt if the order is not tracked.
  struct FillResult {
    OrderMeta meta{};  // a copy: the entry may have just been erased
    OrderQuantity leaves{0};
    bool final{false};
  };
  bool applyFill(OrderId id, OrderQuantity quantity, FillResult& out);

  // Removes and hands back the metadata. Used by cancel and amend.
  bool erase(OrderId id, OrderMeta& out);

  bool resolveCoid(SessionCoid key, OrderId& out) const;
  bool coidInUse(SessionCoid key) const { return m_by_coid.contains(key); }

  // Every live order id belonging to a session, for cancel-on-disconnect.
  std::vector<OrderId> idsForSession(uint32_t session_id) const;

  std::size_t size() const noexcept { return m_orders.size(); }

 private:
  std::unordered_map<OrderId, OrderMeta> m_orders{};
  std::unordered_map<SessionCoid, OrderId> m_by_coid{};
};
}  // namespace Exchange::Net
