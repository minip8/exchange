#include "net/io/Listener.hpp"

#include <boost/asio/ip/address.hpp>

#include "net/io/Log.hpp"

namespace Exchange::Net {
Listener::Listener(IoThread& thread, const std::string& host, uint16_t port,
                   Handler handler)
    : m_thread(thread),
      m_host(host),
      m_port(port),
      m_handler(std::move(handler)),
      m_acceptor(thread.context()) {}

bool Listener::listen(std::string& error) {
  boost::system::error_code ec{};
  const auto address{asio::ip::make_address(m_host, ec)};
  if (ec) {
    error = "bad bind address " + m_host + ": " + ec.message();
    return false;
  }

  const tcp::endpoint endpoint{address, m_port};
  m_acceptor.open(endpoint.protocol(), ec);
  if (ec) {
    error = "open: " + ec.message();
    return false;
  }
  // So a restart after a crash is not blocked by lingering TIME_WAIT sockets.
  m_acceptor.set_option(asio::socket_base::reuse_address{true}, ec);
  m_acceptor.bind(endpoint, ec);
  if (ec) {
    error =
        "bind " + m_host + ":" + std::to_string(m_port) + ": " + ec.message();
    return false;
  }
  m_acceptor.listen(asio::socket_base::max_listen_connections, ec);
  if (ec) {
    error = "listen: " + ec.message();
    return false;
  }

  doAccept();
  return true;
}

void Listener::doAccept() {
  if (m_stopped) return;
  auto self{shared_from_this()};
  m_acceptor.async_accept(
      [this, self](const boost::system::error_code& ec, tcp::socket socket) {
        if (m_stopped) return;
        if (ec) {
          // An accept failure is per-connection (fd exhaustion, a peer that
          // vanished mid-handshake); keep the listener armed.
          if (ec != asio::error::operation_aborted) {
            logError("accept on {}: {}", m_port, ec.message());
            doAccept();
          }
          return;
        }
        m_handler(std::move(socket));
        doAccept();
      });
}

uint16_t Listener::boundPort() const {
  boost::system::error_code ec{};
  const auto endpoint{m_acceptor.local_endpoint(ec)};
  return ec ? m_port : endpoint.port();
}

void Listener::stop() {
  m_stopped = true;
  boost::system::error_code ec{};
  m_acceptor.close(ec);
}
}  // namespace Exchange::Net
