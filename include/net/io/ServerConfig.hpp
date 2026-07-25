#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace Exchange::Net {
struct ServerConfig {
  // Custom binary protocol for algo clients. Bound to loopback only — it is
  // never exposed through the tunnel, so only the HTTP port faces the world.
  uint16_t binary_port{9001};
  std::string binary_bind{"127.0.0.1"};

  // HTTP + WebSocket for the browser GUI. This is the port Cloudflare Tunnel
  // fronts, so it binds all interfaces.
  uint16_t http_port{8080};
  std::string http_bind{"0.0.0.0"};

  std::size_t io_threads{1};

  /*
  How long the matching thread spins with cpuPause() before falling back to
  yield() and then a short sleep. 0 by default: a permanently hot core is not
  something to inflict on a laptop. Set ~100 when benchmarking — the
  difference that makes is one of the more instructive numbers here.
  */
  std::uint32_t spin_us{0};

  std::string traders_path{"config/traders.json"};
  std::string web_root{"web"};
};
}  // namespace Exchange::Net
