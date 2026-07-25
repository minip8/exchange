#include "net/io/Server.hpp"

#include "net/io/HttpSession.hpp"
#include "net/io/Log.hpp"
#include "net/io/TcpSession.hpp"

namespace Exchange::Net {
Server::Server(ServerConfig config) : m_config(std::move(config)) {}

Server::~Server() { stop(); }

bool Server::start(std::string& error) {
  if (m_running) return true;

  if (!m_traders.load(m_config.traders_path, error)) return false;
  logLine("loaded {} trader credential(s) from {}", m_traders.size(),
          m_config.traders_path);

  const std::size_t threads{m_config.io_threads == 0 ? 1 : m_config.io_threads};
  // 256 is the hard ceiling: the I/O thread index is the high byte of a
  // session id, which is what makes egress routing an array index.
  if (threads > 256) {
    error = "--io-threads must be at most 256 (the session id high byte)";
    return false;
  }

  // Construction order is forced: the matching thread owns the ingress
  // rings, an I/O thread needs its ring to exist, and the matching thread
  // needs a sink. So sink first (holding a reference to the still-empty
  // thread vector), then the matching thread, then the threads themselves.
  m_sink = std::make_unique<PostingEventSink>(m_io_threads, threads);
  m_matching = std::make_unique<MatchingThread>(
      *m_sink, threads, MatchingLoopConfig{}, m_config.spin_us);

  m_io_threads.reserve(threads);
  for (std::size_t i{0}; i < threads; ++i) {
    m_io_threads.push_back(std::make_unique<IoThread>(i, m_matching->ring(i)));
  }

  // Accepting happens on thread 0 only.
  m_binary_listener = std::make_shared<Listener>(
      *m_io_threads[0], m_config.binary_bind, m_config.binary_port,
      [this](tcp::socket socket) { acceptBinary(std::move(socket)); });
  if (!m_binary_listener->listen(error)) return false;

  m_http_listener = std::make_shared<Listener>(
      *m_io_threads[0], m_config.http_bind, m_config.http_port,
      [this](tcp::socket socket) { acceptHttp(std::move(socket)); });
  if (!m_http_listener->listen(error)) return false;

  m_matching->start();
  for (auto& thread : m_io_threads) thread->start();

  m_running = true;
  logLine("binary  {}:{} (loopback only)", m_config.binary_bind,
          m_binary_listener->boundPort());
  logLine("http/ws {}:{} (serving {})", m_config.http_bind,
          m_http_listener->boundPort(), m_config.web_root);
  logLine("io threads {}, spin {}us", threads, m_config.spin_us);
  return true;
}

std::size_t Server::nextIoThread() noexcept {
  const std::size_t index{m_next_io_thread};
  m_next_io_thread = (m_next_io_thread + 1) % m_io_threads.size();
  return index;
}

/*
Hands a freshly accepted socket to the thread that will own it.

Accepting happens on thread 0, but a session must be pinned to one thread for
its whole life — that pinning is what removes every strand and lets
per-session counters be plain integers. So the descriptor is released from
the accepting context and re-adopted on the target one.

Moving the socket object alone is NOT sufficient and misbehaves silently: a
moved-to socket keeps its association with the reactor that opened it, so
completions would be delivered on the accepting thread while the session
believed it owned them. release() + re-adopt is the only correct form.

`release()` requires that no operation is outstanding, which is exactly true
here — the socket has just been accepted and nothing has been started on it.
*/
void Server::handOff(tcp::socket socket, std::size_t target,
                     std::function<void(tcp::socket, IoThread&)> make) {
  IoThread& owner{*m_io_threads[target]};
  if (target == 0) {
    // Already on the accepting thread; no handoff, no post.
    make(std::move(socket), owner);
    return;
  }

  boost::system::error_code ec{};
  const auto protocol{socket.local_endpoint(ec).protocol()};
  if (ec) return;  // the peer vanished between accept and here
  const auto descriptor{socket.release(ec)};
  if (ec) return;

  asio::post(owner.context(),
             [&owner, descriptor, protocol, make = std::move(make)]() mutable {
               boost::system::error_code adopt_ec{};
               tcp::socket adopted{owner.context()};
               adopted.assign(protocol, descriptor, adopt_ec);
               if (adopt_ec) return;
               make(std::move(adopted), owner);
             });
}

void Server::acceptBinary(tcp::socket socket) {
  handOff(std::move(socket), nextIoThread(),
          [this](tcp::socket owned, IoThread& owner) {
            const uint32_t session_id{owner.nextSessionId()};
            auto session{std::make_shared<TcpSession>(
                std::move(owned),
                SessionContext{
                    .io = owner, .ring = owner.ring(), .traders = m_traders},
                session_id)};
            owner.sessions().add(session_id, session);
            session->start();
          });
}

/*
One listener serves both the GUI and its WebSocket. Whether a connection is a
WebSocket is a property of its first request, not of the port, so the decision
belongs in HttpSession rather than here.
*/
void Server::acceptHttp(tcp::socket socket) {
  handOff(std::move(socket), nextIoThread(),
          [this](tcp::socket owned, IoThread& owner) {
            std::make_shared<HttpSession>(
                std::move(owned),
                SessionContext{
                    .io = owner, .ring = owner.ring(), .traders = m_traders},
                m_config.web_root)
                ->start();
          });
}

void Server::stop() {
  if (!m_running) return;
  m_running = false;

  // Order matters. Stop accepting, then stop the I/O threads so nothing more
  // enters the ingress rings, and only then stop the matching thread — which
  // drains whatever is still queued rather than dropping commands a client
  // was already told had been accepted.
  if (m_binary_listener) m_binary_listener->stop();
  if (m_http_listener) m_http_listener->stop();
  for (auto& thread : m_io_threads) thread->stop();
  for (auto& thread : m_io_threads) thread->join();
  if (m_matching) m_matching->stop();

  logLine("handled {} command(s)",
          m_matching ? m_matching->commandsHandled() : 0);
}

uint16_t Server::binaryPort() const {
  return m_binary_listener ? m_binary_listener->boundPort()
                           : m_config.binary_port;
}

uint16_t Server::httpPort() const {
  return m_http_listener ? m_http_listener->boundPort() : m_config.http_port;
}
}  // namespace Exchange::Net
