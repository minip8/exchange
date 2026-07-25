/*
exchange_server — the networked front end for the matching engine.

Argument parsing and signal handling only; everything else lives in
net/io/Server.hpp, which owns the startup and (more importantly) the
shutdown order.
*/
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <charconv>
#include <cstdlib>
#include <exception>
#include <print>
#include <string>
#include <string_view>

#include "net/io/Server.hpp"
#include "net/io/ServerConfig.hpp"

namespace asio = boost::asio;
using Exchange::Net::Server;
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
    } else if (arg == "--binary-bind") {
      if (!next(value)) return false;
      config.binary_bind = std::string{value};
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
          "usage: exchange_server [--binary-port N] [--binary-bind ADDR]\n"
          "                       [--http-port N] [--io-threads N]\n"
          "                       [--spin-us N] [--traders PATH]\n"
          "                       [--web-root PATH]");
      std::exit(0);
    } else {
      std::println(stderr, "unknown argument: {}", arg);
      return false;
    }
  }
  return true;
}
}  // namespace

int main(int argc, char** argv) {
  ServerConfig config{};
  if (!parseArgs(argc, argv, config)) return 2;

  try {
    Server server{config};
    std::string error{};
    if (!server.start(error)) {
      std::println(stderr, "fatal: {}", error);
      return 1;
    }

    /*
    Signals are handled on the main thread, in a context of their own, not on
    an I/O thread. Server::stop() joins the I/O threads, so a handler running
    on one of them would join itself and deadlock.
    */
    asio::io_context signals_context{1};
    asio::signal_set signals{signals_context, SIGINT, SIGTERM};
    signals.async_wait([&](const boost::system::error_code&, int signal) {
      std::println("signal {} — shutting down", signal);
      server.stop();
      signals_context.stop();
    });
    signals_context.run();
  } catch (const std::exception& error) {
    std::println(stderr, "fatal: {}", error.what());
    return 1;
  }
  return 0;
}
