#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

#include "net/core/Hash.hpp"
#include "net/core/Ids.hpp"
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
  TraderId trader_id{};
  SessionId session_id{};
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
  SessionId session_id{};
  uint64_t client_order_id{};
  bool operator==(const SessionCoid&) const noexcept = default;
};
}  // namespace Exchange::Net

template <>
struct std::hash<Exchange::Net::SessionCoid> {
  size_t operator()(const Exchange::Net::SessionCoid& key) const noexcept {
    // Client order ids are typically dense per session, so the coid is the
    // one scrambled and the session folded in after — mixing them at
    // different bit weights is what keeps buckets spread. See Hash.hpp.
    return Exchange::Net::hashCombine(key.client_order_id,
                                      key.session_id.value);
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

--- Pointer vs. optional, which is a convention across the gateway ---

A raw pointer means BORROWED-OR-ABSENT: it aliases storage this object owns,
it is invalidated by the next mutation, and returning it copies nothing. That
is the right shape for find(), which sits on the order path and would
otherwise copy an OrderMeta on every lookup.

std::optional means OWNED-OR-ABSENT: the value is the caller's, detached from
this object's storage, and safe to hold across mutations. Every method below
that hands back state which the call itself may have just erased returns one.
Those all used to be `bool f(..., T& out)`, which cost the same copy while
letting a caller read `out` after a false return.
*/
class OrderStore {
 public:
  // Returns false if `leaves` is 0 — nothing rested, so nothing is stored.
  bool insert(OrderId id, const OrderMeta& meta);

  // Borrowed: invalidated by the next insert/erase/applyFill.
  const OrderMeta* find(OrderId id) const;

  struct FillResult {
    OrderMeta meta{};
    OrderQuantity leaves{0};
    bool final{false};
  };

  // Applies one fill leg. The entry is erased when `leaves` reaches 0, which
  // is why the result carries a detached copy of the metadata rather than a
  // pointer to it. nullopt means the order is not tracked here.
  std::optional<FillResult> applyFill(OrderId id, OrderQuantity quantity);

  // Removes and hands back the metadata. Used by cancel and amend.
  std::optional<OrderMeta> erase(OrderId id);

  std::optional<OrderId> resolveCoid(SessionCoid key) const;
  bool coidInUse(SessionCoid key) const { return m_by_coid.contains(key); }

  // Every live order id belonging to a session, for cancel-on-disconnect.
  std::vector<OrderId> idsForSession(SessionId session_id) const;

  std::size_t size() const noexcept { return m_orders.size(); }

 private:
  std::unordered_map<OrderId, OrderMeta> m_orders{};
  std::unordered_map<SessionCoid, OrderId> m_by_coid{};
};
}  // namespace Exchange::Net
