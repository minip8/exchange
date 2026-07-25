#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Exchange::Net {
class TcpSession;

/*
The sessions belonging to one I/O thread.

No mutex, and none is needed: a session is pinned to the thread that accepted
it for its entire life, and only that thread ever touches this table. That
pinning is the same property that lets per-session counters be plain
integers, and it is why there are no strands anywhere in this design.

Entries are weak: a session stays alive because an outstanding async
operation holds a shared_ptr to it (the usual Asio ownership shape). When the
last handler completes, the session dies and the entry here is already stale,
so `find` prunes as it goes.
*/
class SessionTable {
 public:
  void add(uint32_t session_id, const std::shared_ptr<TcpSession>& session) {
    m_sessions.insert_or_assign(session_id, session);
  }

  void remove(uint32_t session_id) { m_sessions.erase(session_id); }

  std::shared_ptr<TcpSession> find(uint32_t session_id) {
    const auto it{m_sessions.find(session_id)};
    if (it == m_sessions.end()) return nullptr;
    auto session{it->second.lock()};
    if (!session) m_sessions.erase(it);
    return session;
  }

  // Snapshots into a vector rather than exposing the map, so a callback may
  // safely close sessions (which mutates the table) while iterating.
  std::vector<std::shared_ptr<TcpSession>> all() {
    std::vector<std::shared_ptr<TcpSession>> live{};
    live.reserve(m_sessions.size());
    for (auto it{m_sessions.begin()}; it != m_sessions.end();) {
      auto session{it->second.lock()};
      if (session) {
        live.push_back(std::move(session));
        ++it;
      } else {
        it = m_sessions.erase(it);
      }
    }
    return live;
  }

  std::size_t size() const noexcept { return m_sessions.size(); }

 private:
  std::unordered_map<uint32_t, std::weak_ptr<TcpSession>> m_sessions{};
};
}  // namespace Exchange::Net
