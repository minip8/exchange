#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Exchange::Net {
struct Trader {
  uint32_t id{};
  std::string name{};
};

/*
API key -> trader identity, loaded once at startup and never mutated.

Immutability is the whole synchronization story: every I/O thread reads this
concurrently, and because nothing ever writes after `load` returns, no lock
and no atomics are needed. If credential reloading is ever wanted, it must be
a whole new instance published behind a shared_ptr, not an in-place edit.

Authentication runs on the I/O thread, and only the resolved trader_id
crosses the ring — the key itself never reaches the matching thread.
*/
class TraderDirectory {
 public:
  // Returns false (with `error` set) if the file is missing or malformed.
  // A server with no credentials cannot authenticate anyone, so this is
  // fatal at startup rather than a warning.
  bool load(const std::string& path, std::string& error);

  // Constant-time in the length of the key: a timing-distinguishable compare
  // over an unordered_map lookup would leak key prefixes to anyone who can
  // measure a few thousand logons. Cheap enough to just do properly.
  const Trader* authenticate(std::string_view api_key) const;

  std::size_t size() const noexcept { return m_by_key.size(); }

 private:
  std::unordered_map<std::string, Trader> m_by_key{};
};

/*
Per-IP logon attempt limiting.

Deliberately per-I/O-thread rather than global: a shared counter would need a
mutex on the accept path to defend against something a handful of friends on
a Cloudflare tunnel will never do. With N threads the effective limit is N
times the configured one, which is the right trade at this scale and is
stated here so nobody later reads the number as exact.
*/
class LogonAttemptLimiter {
 public:
  explicit LogonAttemptLimiter(uint32_t max_failures_per_window = 10,
                               uint64_t window_ns = 60'000'000'000ull)
      : m_max_failures(max_failures_per_window), m_window_ns(window_ns) {}

  bool allowed(uint32_t ip, uint64_t now_ns) const;
  void recordFailure(uint32_t ip, uint64_t now_ns);
  void recordSuccess(uint32_t ip);

 private:
  struct Attempts {
    uint32_t failures{};
    uint64_t window_start_ns{};
  };
  uint32_t m_max_failures;
  uint64_t m_window_ns;
  mutable std::unordered_map<uint32_t, Attempts> m_by_ip{};
};
}  // namespace Exchange::Net
