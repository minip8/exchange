#include "net/io/Server.hpp"

#include <print>

#include "net/io/HttpSession.hpp"
#include "net/io/TcpSession.hpp"

namespace Exchange::Net {
Server::Server(ServerConfig config) : m_config(std::move(config)) {}

Server::~Server() { stop(); }

bool Server::start(std::string& error) {
  if (m_running) return true;

  if (!m_traders.load(m_config.traders_path, error)) return false;
  std::println("loaded {} trader credential(s) from {}", m_traders.size(),
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
  std::println("binary  {}:{} (loopback only)", m_config.binary_bind,
               m_binary_listener->boundPort());
  std::println("http/ws {}:{} (serving {})", m_config.http_bind,
               m_http_listener->boundPort(), m_config.web_root);
  std::println("io threads {}, spin {}us", threads, m_config.spin_us);
  return true;
}

void Server::acceptBinary(tcp::socket socket) {
  // Phase 2 runs one I/O thread, so the accepting thread is also the owning
  // thread and no handoff is needed. Phase 5 adds release()/adopt() here.
  IoThread& owner{*m_io_threads[0]};
  const uint32_t session_id{owner.nextSessionId()};
  auto session{std::make_shared<TcpSession>(
      std::move(socket),
      SessionContext{.io = owner, .ring = owner.ring(), .traders = m_traders},
      session_id)};
  owner.sessions().add(session_id, session);
  session->start();
}

/*
One listener serves both the GUI and its WebSocket. Whether a connection is a
WebSocket is a property of its first request, not of the port, so the decision
belongs in HttpSession rather than here.
*/
void Server::acceptHttp(tcp::socket socket) {
  IoThread& owner{*m_io_threads[0]};
  std::make_shared<HttpSession>(
      std::move(socket),
      SessionContext{.io = owner, .ring = owner.ring(), .traders = m_traders},
      m_config.web_root)
      ->start();
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

  std::println("handled {} command(s)",
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
