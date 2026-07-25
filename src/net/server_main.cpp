/*
exchange_server — the networked front end for the matching engine.

Phase 0: binds both listening sockets and waits for SIGINT. The point of
shipping this first is that it forces the CMake-4.4 / CONFIG-mode-Boost
question and the separate-compilation build times to be answered on day one,
before any protocol code exists to obscure them.
*/
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>
#include <charconv>
#include <cstdlib>
#include <exception>
#include <print>
#include <string_view>

#include "net/io/ServerConfig.hpp"

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using Exchange::Net::ServerConfig;

namespace {
bool parseUint(std::string_view text, auto& out) {
  const auto* const end{text.data() + text.size()};
  const auto result{std::from_chars(text.data(), end, out)};
  return result.ec == std::errc{} && result.ptr == end;
}

// Returns false if the arguments were malformed (the caller exits non-zero).
bool parseArgs(int argc, char** argv, ServerConfig& config) {
  for (int i{1}; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    auto next{[&](std::string_view& out) {
      if (i + 1 >= argc) return false;
      out = std::string_view{argv[++i]};
      return true;
    }};
    std::string_view value{};

    if (arg == "--binary-port") {
      if (!next(value) || !parseUint(value, config.binary_port)) return false;
    } else if (arg == "--http-port") {
      if (!next(value) || !parseUint(value, config.http_port)) return false;
    } else if (arg == "--io-threads") {
      if (!next(value) || !parseUint(value, config.io_threads) ||
          config.io_threads == 0) {
        return false;
      }
    } else if (arg == "--spin-us") {
      if (!next(value) || !parseUint(value, config.spin_us)) return false;
    } else if (arg == "--traders") {
      if (!next(value)) return false;
      config.traders_path = std::string{value};
    } else if (arg == "--web-root") {
      if (!next(value)) return false;
      config.web_root = std::string{value};
    } else if (arg == "--help" || arg == "-h") {
      std::println(
          "usage: exchange_server [--binary-port N] [--http-port N]\n"
          "                       [--io-threads N] [--spin-us N]\n"
          "                       [--traders PATH] [--web-root PATH]");
      std::exit(0);
    } else {
      std::println(stderr, "unknown argument: {}", arg);
      return false;
    }
  }
  return true;
}

// Bound with reuse_address so a restart after a crash is not blocked by
// lingering TIME_WAIT sockets.
tcp::acceptor bindListener(asio::io_context& io, std::string_view host,
                           uint16_t port) {
  const tcp::endpoint endpoint{asio::ip::make_address(host), port};
  tcp::acceptor acceptor{io, endpoint.protocol()};
  acceptor.set_option(asio::socket_base::reuse_address{true});
  acceptor.bind(endpoint);
  acceptor.listen(asio::socket_base::max_listen_connections);
  return acceptor;
}
}  // namespace

int main(int argc, char** argv) {
  ServerConfig config{};
  if (!parseArgs(argc, argv, config)) return 2;

  try {
    asio::io_context io{static_cast<int>(config.io_threads)};

    tcp::acceptor binary{
        bindListener(io, config.binary_bind, config.binary_port)};
    tcp::acceptor http{bindListener(io, config.http_bind, config.http_port)};

    std::println("binary  {}:{}", config.binary_bind, config.binary_port);
    std::println("http/ws {}:{}", config.http_bind, config.http_port);
    std::println("io threads {}, spin {}us", config.io_threads, config.spin_us);

    asio::signal_set signals{io, SIGINT, SIGTERM};
    signals.async_wait([&](const boost::system::error_code&, int signal) {
      std::println("signal {} — shutting down", signal);
      io.stop();
    });

    io.run();
  } catch (const std::exception& error) {
    std::println(stderr, "fatal: {}", error.what());
    return 1;
  }
  return 0;
}
