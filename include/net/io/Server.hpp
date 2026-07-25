#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "net/gateway/MatchingThread.hpp"
#include "net/io/IoThread.hpp"
#include "net/io/Listener.hpp"
#include "net/io/PostingEventSink.hpp"
#include "net/io/ServerConfig.hpp"
#include "net/io/TraderDirectory.hpp"

namespace Exchange::Net {
/*
Wires the whole thing together and owns the shutdown order, which is the part
that actually matters:

  1. listeners stop accepting,
  2. I/O threads stop, so nothing new is pushed into the ingress rings,
  3. the matching thread stops, draining whatever is still queued,
  4. only then does anything get destroyed.

Doing it in any other order lets the matching thread publish into an
io_context that is being torn down, or drops commands the client was already
told had been accepted.
*/
class Server {
 public:
  explicit Server(ServerConfig config);
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;
  ~Server();

  // Returns false with `error` set if credentials could not be loaded or a
  // port could not be bound. Both are fatal at startup rather than warnings.
  bool start(std::string& error);
  void stop();

  // The port actually bound, which differs from the configured one when the
  // config asked for 0. Used by the loopback tests.
  uint16_t binaryPort() const;
  uint16_t httpPort() const;

 private:
  void acceptBinary(tcp::socket socket);
  void acceptHttp(tcp::socket socket);
  // Moves a freshly accepted socket to the I/O thread that will own it, then
  // calls `make` on that thread. See the comment on the definition.
  void handOff(tcp::socket socket, std::size_t target,
               std::function<void(tcp::socket, IoThread&)> make);
  std::size_t nextIoThread() noexcept;

  ServerConfig m_config;
  TraderDirectory m_traders{};
  std::vector<std::unique_ptr<IoThread>> m_io_threads{};
  std::unique_ptr<PostingEventSink> m_sink{};
  std::unique_ptr<MatchingThread> m_matching{};
  std::shared_ptr<Listener> m_binary_listener{};
  std::shared_ptr<Listener> m_http_listener{};
  // Round-robin, not least-loaded: with a handful of long-lived sessions
  // the difference is noise, and a load metric would need to be shared
  // across threads to be read here.
  std::size_t m_next_io_thread{0};
  bool m_running{false};
};
}  // namespace Exchange::Net
