#include "net/io/TcpSession.hpp"

#include <boost/asio/write.hpp>
#include <chrono>
#include <cstring>
#include <print>

#include "net/wire/BinaryProtocol.hpp"

namespace Exchange::Net {
namespace {
// 64 KiB in, which is many frames per syscall. The maximum frame is 1 KiB,
// so the parser can always make progress even in the worst case.
constexpr std::size_t kReadBufferSize{64 * 1024};
constexpr auto kRetryInterval{std::chrono::microseconds{200}};

uint64_t nowNs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}
}  // namespace

TcpSession::TcpSession(tcp::socket socket, SessionContext context,
                       uint32_t session_id)
    : m_socket(std::move(socket)),
      m_context(context),
      m_session_id(session_id),
      m_read_buffer(kReadBufferSize),
      m_retry_timer(m_socket.get_executor()) {
  boost::system::error_code ec{};
  const auto endpoint{m_socket.remote_endpoint(ec)};
  if (!ec && endpoint.address().is_v4()) {
    m_peer_ip = endpoint.address().to_v4().to_uint();
  }
  // Order flow is small and latency-sensitive; Nagle would hold an ack back
  // waiting for something to coalesce with.
  m_socket.set_option(tcp::no_delay{true}, ec);
}

void TcpSession::start() { doRead(); }

void TcpSession::doRead() {
  if (m_closing || m_flush_then_close || m_reads_suspended) return;
  if (m_read_size >= m_read_buffer.size()) {
    close("read buffer exhausted");
    return;
  }
  auto self{shared_from_this()};
  m_socket.async_read_some(
      asio::buffer(m_read_buffer.data() + m_read_size,
                   m_read_buffer.size() - m_read_size),
      [this, self](const boost::system::error_code& ec, std::size_t bytes) {
        if (ec) {
          close(ec == asio::error::eof ? "peer closed" : ec.message());
          return;
        }
        onRead(bytes);
      });
}

void TcpSession::onRead(std::size_t bytes) {
  m_read_size += bytes;

  std::size_t offset{0};
  while (offset < m_read_size) {
    const std::span<const std::byte> remaining{m_read_buffer.data() + offset,
                                               m_read_size - offset};
    const auto result{
        Binary::decode(remaining, m_session_id, m_trader_id, nowNs())};

    if (result.status == Binary::DecodeStatus::NeedMore) break;
    if (result.status == Binary::DecodeStatus::Malformed) {
      // A bad length or version desynchronizes the stream, and there is no
      // resynchronization point to skip to — so the connection goes.
      close("malformed frame");
      return;
    }
    if (result.status == Binary::DecodeStatus::UnknownType) {
      // Framing was fine, so the stream is still in sync: reject and go on.
      sendReject(RejectCode::MalformedMessage, 0, 0);
      offset += result.consumed;
      continue;
    }

    const std::span<const std::byte> frame{remaining.data(), result.consumed};
    const bool keep_going{
        handleFrame(result.type, frame, result.command, result.seq)};
    offset += result.consumed;
    if (!keep_going) return;
  }

  // Compact whatever partial frame is left to the front of the buffer.
  if (offset > 0) {
    m_read_size -= offset;
    if (m_read_size > 0) {
      std::memmove(m_read_buffer.data(), m_read_buffer.data() + offset,
                   m_read_size);
    }
  }

  doWrite();
  doRead();
}

bool TcpSession::handleFrame(MsgType type, std::span<const std::byte> frame,
                             const Command& command, uint32_t seq) {
  if (type == MsgType::Logon) {
    if (loggedOn()) {
      sendReject(RejectCode::AlreadyLoggedOn, 0, 0);
      return true;
    }
    return authenticate(frame, command);
  }

  if (type == MsgType::Heartbeat) {
    // Answered here. Sending it through the ring would burn an ingress slot
    // per client per interval for something the matching thread cannot act
    // on.
    Binary::encodeSimple(MsgType::ServerHeartbeat, seq, m_write_back);
    return true;
  }

  if (!loggedOn()) {
    sendReject(RejectCode::NotLoggedOn, command.client_order_id,
               command.order_id);
    return true;
  }

  submit(command);
  return true;
}

bool TcpSession::authenticate(std::span<const std::byte> frame,
                              const Command& command) {
  Binary::LogonBody body{};
  if (!Binary::readBody(frame.subspan(Binary::kHeaderSize), body)) {
    close("short logon");
    return false;
  }

  const uint64_t now{nowNs()};
  if (!m_context.io.limiter().allowed(m_peer_ip, now)) {
    sendReject(RejectCode::AuthFailed, 0, 0);
    closeAfterFlush("too many failed logons");
    return false;
  }

  const std::size_t length{::strnlen(body.api_key, sizeof(body.api_key))};
  const Trader* trader{
      m_context.traders.authenticate(std::string_view{body.api_key, length})};
  if (trader == nullptr) {
    m_context.io.limiter().recordFailure(m_peer_ip, now);
    sendReject(RejectCode::AuthFailed, 0, 0);
    closeAfterFlush("authentication failed");
    return false;
  }

  m_context.io.limiter().recordSuccess(m_peer_ip);
  m_trader_id = trader->id;

  // Only the resolved identity crosses the ring; the key never does.
  Command opened{command};
  opened.trader_id = m_trader_id;
  submit(opened);

  std::println("session {:#x} logged on as trader {} ({})", m_session_id,
               trader->id, trader->name);
  return true;
}

void TcpSession::submit(const Command& command) {
  if (command.type == CommandType::None) return;

  // Order matters: anything already queued goes first, or this session's
  // commands would reorder relative to each other. Per-session FIFO is the
  // one ordering guarantee the design promises, and the reason a
  // single-producer ring per I/O thread is sufficient.
  if (m_pending.empty() && m_context.ring.tryPush(command)) return;

  m_pending.push_back(command);
  if (!m_reads_suspended) {
    m_reads_suspended = true;
    retryPending();
  }
}

void TcpSession::retryPending() {
  while (!m_pending.empty() && m_context.ring.tryPush(m_pending.front())) {
    m_pending.pop_front();
  }
  if (m_pending.empty()) {
    if (m_reads_suspended) {
      m_reads_suspended = false;
      doRead();
    }
    return;
  }

  auto self{shared_from_this()};
  m_retry_timer.expires_after(kRetryInterval);
  m_retry_timer.async_wait([this, self](const boost::system::error_code& ec) {
    if (ec || m_closing) return;
    retryPending();
  });
}

void TcpSession::sendReject(RejectCode code, uint64_t client_order_id,
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
  append(event);
}

void TcpSession::append(const Event& event) {
  Binary::encode(event, ++m_out_seq, m_write_back);
}

void TcpSession::deliver(std::span<const Event> events) {
  if (m_closing) return;
  for (const Event& event : events) append(event);
  doWrite();
}

void TcpSession::doWrite() {
  if (m_closing) return;
  if (m_writing) return;
  if (m_write_back.empty()) {
    if (m_flush_then_close) close(m_close_reason);
    return;
  }

  m_write_front.swap(m_write_back);
  m_write_back.clear();
  m_writing = true;

  auto self{shared_from_this()};
  asio::async_write(
      m_socket, asio::buffer(m_write_front),
      [this, self](const boost::system::error_code& ec, std::size_t) {
        m_writing = false;
        if (ec) {
          close(ec.message());
          return;
        }
        // Whatever accumulated while that write was in flight goes out as
        // one more write, not one write per event.
        doWrite();
      });
}

void TcpSession::closeAfterFlush(std::string_view reason) {
  if (m_closing || m_flush_then_close) return;
  m_flush_then_close = true;
  m_close_reason.assign(reason);
  doWrite();
}

void TcpSession::close(std::string_view reason) {
  if (m_closing) return;
  m_closing = true;

  /*
  The matching thread must learn the session is gone, or its routing entry
  and every subscription it holds leak forever. This is one of the two
  commands that may never be dropped — so if the ring is full it is handed to
  the I/O thread's critical queue, which keeps retrying after this session
  object is gone. Blocking here instead would stall every other session on
  this thread.
  */
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
  m_retry_timer.cancel();
  m_socket.shutdown(tcp::socket::shutdown_both, ec);
  m_socket.close(ec);
  m_context.io.sessions().remove(m_session_id);

  std::println("session {:#x} closed: {}", m_session_id, reason);
}
}  // namespace Exchange::Net
