#include "net/io/WebSocketSession.hpp"

#include <boost/beast/core/bind_handler.hpp>
#include <chrono>
#include <print>

#include "net/wire/JsonProtocol.hpp"

namespace Exchange::Net {
namespace {
uint64_t nowNs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}
}  // namespace

WebSocketSession::WebSocketSession(tcp::socket socket, SessionContext context,
                                   uint32_t session_id)
    : m_stream(std::move(socket)),
      m_context(context),
      m_session_id(session_id) {
  boost::system::error_code ec{};
  const auto endpoint{m_stream.next_layer().remote_endpoint(ec)};
  if (!ec && endpoint.address().is_v4()) {
    m_peer_ip = endpoint.address().to_v4().to_uint();
  }
  m_pump.emplace(m_context.ring, m_stream.get_executor(), [this] { doRead(); });
}

void WebSocketSession::run(
    boost::beast::http::request<boost::beast::http::string_body> request) {
  m_stream.set_option(websocket::stream_base::timeout::suggested(
      boost::beast::role_type::server));
  m_stream.set_option(
      websocket::stream_base::decorator([](websocket::response_type& response) {
        response.set(boost::beast::http::field::server, "exchange");
      }));
  // Text, not binary: the payload is JSON, and a browser reading it in the
  // network tab is a feature at this scale.
  m_stream.text(true);

  auto self{shared_from_this()};
  m_stream.async_accept(request,
                        [this, self](const boost::system::error_code& ec) {
                          if (ec) {
                            close("websocket handshake: " + ec.message());
                            return;
                          }
                          m_context.io.sessions().add(m_session_id, self);
                          doRead();
                        });
}

void WebSocketSession::doRead() {
  if (m_closing || m_pump->readsSuspended()) return;
  auto self{shared_from_this()};
  m_stream.async_read(
      m_buffer, [this, self](const boost::system::error_code& ec, std::size_t) {
        if (ec) {
          close(ec == websocket::error::closed ? "peer closed" : ec.message());
          return;
        }
        const auto data{m_buffer.data()};
        onMessage(std::string_view{static_cast<const char*>(data.data()),
                                   data.size()});
        m_buffer.consume(m_buffer.size());
        doRead();
      });
}

void WebSocketSession::onMessage(std::string_view text) {
  auto result{Json::decode(text, m_session_id, m_trader_id, nowNs())};

  if (result.status == Json::JsonStatus::Malformed) {
    sendReject(RejectCode::MalformedMessage, 0, 0);
    return;
  }
  if (result.status == Json::JsonStatus::UnknownType) {
    sendReject(RejectCode::MalformedMessage, 0, 0);
    return;
  }

  if (result.type == MsgType::Logon) {
    if (loggedOn()) {
      sendReject(RejectCode::AlreadyLoggedOn, 0, 0);
      return;
    }
    const AuthOutcome outcome{
        authenticateSession(m_context.traders, m_context.io.limiter(),
                            m_peer_ip, result.api_key, nowNs())};
    if (!outcome.ok) {
      sendReject(outcome.code, 0, 0);
      // Written before the close, unlike a hard tear-down: a browser that
      // typed the wrong key deserves to be told which failure it was.
      close("authentication failed");
      return;
    }
    m_trader_id = outcome.trader_id;
    result.command.trader_id = m_trader_id;
    m_pump->submit(result.command);
    std::println("ws session {:#x} logged on as trader {} ({})", m_session_id,
                 outcome.trader_id, outcome.name);
    return;
  }

  if (result.type == MsgType::Heartbeat) {
    enqueue(R"({"type":"server_heartbeat"})");
    return;
  }

  if (!loggedOn()) {
    sendReject(RejectCode::NotLoggedOn, result.command.client_order_id,
               result.command.order_id);
    return;
  }

  m_pump->submit(result.command);
}

void WebSocketSession::sendReject(RejectCode code, uint64_t client_order_id,
                                  uint64_t order_id) {
  Event event{};
  event.session_id = m_session_id;
  event.trader_id = m_trader_id;
  event.type = EventType::Reject;
  event.payload.ack = AckPayload{.order_id = order_id,
                                 .client_order_id = client_order_id,
                                 .orig_order_id = 0,
                                 .price = 0,
                                 .quantity = 0,
                                 .reject_code = toWire(code),
                                 .pad = {}};
  enqueue(Json::encode(event));
}

void WebSocketSession::deliver(std::span<const Event> events) {
  if (m_closing) return;
  for (const Event& event : events) {
    std::string text{Json::encode(event)};
    if (!text.empty()) m_outbox.push_back(std::move(text));
  }
  doWrite();
}

void WebSocketSession::enqueue(std::string message) {
  if (message.empty() || m_closing) return;
  m_outbox.push_back(std::move(message));
  doWrite();
}

void WebSocketSession::doWrite() {
  // Beast permits exactly one outstanding write per stream, which is why
  // this queues rather than coalescing into one buffer the way the binary
  // session does.
  if (m_writing || m_closing || m_outbox.empty()) return;
  m_writing = true;
  auto self{shared_from_this()};
  m_stream.async_write(
      asio::buffer(m_outbox.front()),
      [this, self](const boost::system::error_code& ec, std::size_t) {
        m_writing = false;
        if (ec) {
          close(ec.message());
          return;
        }
        m_outbox.pop_front();
        doWrite();
      });
}

void WebSocketSession::close(std::string_view reason) {
  if (m_closing) return;
  m_closing = true;
  m_pump->cancel();

  // Same rule as the binary session: the matching thread must learn the
  // session is gone or its routing and subscriptions leak. If the ring is
  // full the I/O thread keeps retrying after this object is destroyed.
  if (loggedOn()) {
    Command closed{};
    closed.type = CommandType::SessionClosed;
    closed.session_id = m_session_id;
    closed.trader_id = m_trader_id;
    closed.recv_ts_ns = nowNs();
    if (!m_context.ring.tryPush(closed)) {
      m_context.io.enqueueCritical(closed);
    }
  }

  boost::system::error_code ec{};
  m_stream.next_layer().shutdown(tcp::socket::shutdown_both, ec);
  m_stream.next_layer().close(ec);
  m_context.io.sessions().remove(m_session_id);
  std::println("ws session {:#x} closed: {}", m_session_id, reason);
}
}  // namespace Exchange::Net
