#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "net/io/IoThread.hpp"

namespace Exchange::Net {
using tcp = asio::ip::tcp;

/*
Accepts connections on one endpoint and hands each socket to a callback.

Accepting happens on I/O thread 0 only. With more than one I/O thread the
socket is then handed off — see Server::handOff, which uses release() plus
re-adopt on the target context rather than moving the socket object, because
moving alone leaves the descriptor registered with the wrong reactor.
*/
class Listener : public std::enable_shared_from_this<Listener> {
 public:
  using Handler = std::function<void(tcp::socket)>;

  Listener(IoThread& thread, const std::string& host, uint16_t port,
           Handler handler);

  // Binds and arms the accept loop. Returns false with `error` set if the
  // endpoint could not be bound, so startup can fail loudly.
  bool listen(std::string& error);
  void stop();

  // The port actually bound, which differs from the requested one when 0
  // was asked for. Only valid after listen() succeeds.
  uint16_t boundPort() const;

 private:
  void doAccept();

  IoThread& m_thread;
  std::string m_host;
  uint16_t m_port;
  Handler m_handler;
  tcp::acceptor m_acceptor;
  bool m_stopped{false};
};
}  // namespace Exchange::Net
