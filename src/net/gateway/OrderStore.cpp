#include "net/gateway/OrderStore.hpp"

#include <algorithm>

namespace Exchange::Net {
bool OrderStore::insert(OrderId id, const OrderMeta& meta) {
  // Mirrors MatchingEngine::addOrder, which only indexes an order if it
  // actually rested. A fully-filled aggressor was never in the book, so a
  // later cancel of it must answer "unknown" — which it will, because there
  // is nothing here to find.
  if (meta.leaves == OrderQuantity{0}) return false;
  m_orders.insert_or_assign(id, meta);
  m_by_coid.insert_or_assign(SessionCoid{meta.session_id, meta.client_order_id},
                             id);
  return true;
}

const OrderMeta* OrderStore::find(OrderId id) const {
  const auto it{m_orders.find(id)};
  return it == m_orders.end() ? nullptr : &it->second;
}

std::optional<OrderStore::FillResult> OrderStore::applyFill(
    OrderId id, OrderQuantity quantity) {
  const auto it{m_orders.find(id)};
  if (it == m_orders.end()) return std::nullopt;

  OrderMeta& meta{it->second};
  // The engine never fills more than is resting, so this cannot underflow;
  // clamping anyway keeps a hypothetical accounting bug from turning
  // `leaves` into ~2^64 and wedging the store.
  meta.leaves =
      meta.leaves > quantity ? meta.leaves - quantity : OrderQuantity{0};

  FillResult result{.meta = meta,
                    .leaves = meta.leaves,
                    .final = meta.leaves == OrderQuantity{0}};

  if (result.final) {
    m_by_coid.erase(SessionCoid{meta.session_id, meta.client_order_id});
    m_orders.erase(it);
  }
  return result;
}

std::optional<OrderMeta> OrderStore::erase(OrderId id) {
  const auto it{m_orders.find(id)};
  if (it == m_orders.end()) return std::nullopt;
  OrderMeta meta{it->second};
  m_by_coid.erase(SessionCoid{meta.session_id, meta.client_order_id});
  m_orders.erase(it);
  return meta;
}

std::optional<OrderId> OrderStore::resolveCoid(SessionCoid key) const {
  const auto it{m_by_coid.find(key)};
  if (it == m_by_coid.end()) return std::nullopt;
  return it->second;
}

std::vector<OrderId> OrderStore::idsForSession(SessionId session_id) const {
  std::vector<OrderId> ids{};
  for (const auto& [id, meta] : m_orders) {
    if (meta.session_id == session_id) ids.push_back(id);
  }
  // Bucket order is unspecified, and cancel-on-disconnect must produce the
  // same event stream every run — the gateway's determinism is what makes
  // golden-file regression testing possible later.
  std::ranges::sort(ids);
  return ids;
}
}  // namespace Exchange::Net
