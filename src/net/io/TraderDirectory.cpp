#include "net/io/TraderDirectory.hpp"

#include <boost/json.hpp>
#include <fstream>
#include <sstream>

namespace Exchange::Net {
namespace json = boost::json;

bool TraderDirectory::load(const std::string& path, std::string& error) {
  std::ifstream file{path};
  if (!file) {
    error = "cannot open " + path;
    return false;
  }
  std::ostringstream contents{};
  contents << file.rdbuf();

  json::error_code ec{};
  // NOT brace-initialized. json::value has an initializer_list constructor,
  // so `json::value root{json::parse(...)}` builds a one-element ARRAY
  // containing the parsed document rather than the document itself — and
  // then quietly reports that the file has no "traders" key. The repo's
  // house style is braces everywhere; this type is an exception.
  const json::value root = json::parse(contents.str(), ec);
  if (ec) {
    error = path + ": " + ec.message();
    return false;
  }
  if (!root.is_object() || !root.as_object().contains("traders")) {
    error = path + ": expected an object with a \"traders\" array";
    return false;
  }
  const json::value& traders{root.as_object().at("traders")};
  if (!traders.is_array()) {
    error = path + ": \"traders\" must be an array";
    return false;
  }

  for (const json::value& entry : traders.as_array()) {
    if (!entry.is_object()) {
      error = path + ": each trader must be an object";
      return false;
    }
    const json::object& trader{entry.as_object()};
    if (!trader.contains("id") || !trader.contains("api_key")) {
      error = path + ": each trader needs \"id\" and \"api_key\"";
      return false;
    }
    const auto id{trader.at("id").to_number<uint32_t>()};
    // 0 is the sentinel for "not authenticated" throughout the gateway.
    if (id == 0) {
      error = path + ": trader id 0 is reserved";
      return false;
    }
    std::string key{json::value_to<std::string>(trader.at("api_key"))};
    if (key.empty()) {
      error = path + ": empty api_key";
      return false;
    }
    std::string name{trader.contains("name")
                         ? json::value_to<std::string>(trader.at("name"))
                         : std::string{}};
    if (!m_by_key.emplace(std::move(key), Trader{id, std::move(name)}).second) {
      error = path + ": duplicate api_key";
      return false;
    }
  }

  if (m_by_key.empty()) {
    error = path + ": no traders configured";
    return false;
  }
  return true;
}

const Trader* TraderDirectory::authenticate(std::string_view api_key) const {
  // Every candidate is compared, and each comparison touches every byte, so
  // neither the number of iterations nor the time of one depends on how much
  // of a key matched. `match` is accumulated rather than branched on for the
  // same reason.
  const Trader* found{nullptr};
  for (const auto& [key, trader] : m_by_key) {
    if (key.size() != api_key.size()) continue;
    unsigned char difference{0};
    for (std::size_t i{0}; i < key.size(); ++i) {
      difference = static_cast<unsigned char>(
          difference | (static_cast<unsigned char>(key[i]) ^
                        static_cast<unsigned char>(api_key[i])));
    }
    if (difference == 0) found = &trader;
  }
  return found;
}

bool LogonAttemptLimiter::allowed(uint32_t ip, uint64_t now_ns) const {
  const auto it{m_by_ip.find(ip)};
  if (it == m_by_ip.end()) return true;
  if (now_ns - it->second.window_start_ns >= m_window_ns) return true;
  return it->second.failures < m_max_failures;
}

void LogonAttemptLimiter::recordFailure(uint32_t ip, uint64_t now_ns) {
  Attempts& attempts{m_by_ip[ip]};
  if (attempts.window_start_ns == 0 ||
      now_ns - attempts.window_start_ns >= m_window_ns) {
    attempts.window_start_ns = now_ns;
    attempts.failures = 0;
  }
  ++attempts.failures;
}

void LogonAttemptLimiter::recordSuccess(uint32_t ip) { m_by_ip.erase(ip); }
}  // namespace Exchange::Net
