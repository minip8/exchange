#pragma once

#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "net/core/Event.hpp"
#include "net/io/ClientSession.hpp"
#include "net/io/IoThread.hpp"
#include "net/io/SessionPump.hpp"
#include "net/io/TcpSession.hpp"

namespace Exchange::Net {
namespace websocket = boost::beast::websocket;

/*
A browser client, speaking the same Command/Event vocabulary as the binary
protocol through the JSON codec.

The transport differs (text frames, so no framing to do — the WebSocket layer
already delimits messages) but nothing above it does: the same
SessionPump puts commands on the same ring, the same authenticateSession
checks the key, and the matching thread cannot tell the two apart. That is
the point of having one core and two codecs.

One difference that is not incidental: Beast's websocket stream permits only
one write at a time, so outbound events queue here rather than being
double-buffered into one buffer the way TcpSession does. Events are still
grouped per delivery, so a burst of level updates is a burst of frames but
only one wake-up.
*/
class WebSocketSession : public ClientSession,
                         public std::enable_shared_from_this<WebSocketSession> {
 public:
  WebSocketSession(tcp::socket socket, SessionContext context,
                   uint32_t session_id);

  // Completes the upgrade handshake carried by `request`, then starts.
  void run(
      boost::beast::http::request<boost::beast::http::string_body> request);

  void deliver(std::span<const Event> events) override;
  void close(std::string_view reason) override;

  uint32_t sessionId() const noexcept override { return m_session_id; }
  bool loggedOn() const noexcept { return m_trader_id != 0; }

 private:
  void doRead();
  void onMessage(std::string_view text);
  void doWrite();
  void sendReject(RejectCode code, uint64_t client_order_id, uint64_t order_id);
  void enqueue(std::string message);

  websocket::stream<tcp::socket> m_stream;
  SessionContext m_context;
  uint32_t m_session_id;
  uint32_t m_trader_id{0};
  uint32_t m_peer_ip{0};

  boost::beast::flat_buffer m_buffer{};
  std::deque<std::string> m_outbox{};
  bool m_writing{false};
  bool m_closing{false};

  std::optional<SessionPump> m_pump{};
};
}  // namespace Exchange::Net
