#include "net/io/TcpSession.hpp"

#include <boost/asio/write.hpp>
#include <chrono>
#include <cstring>

#include "net/io/Log.hpp"
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
      m_read_buffer(kReadBufferSize) {
  m_pump.emplace(m_context.io, m_socket.get_executor(), [this] { doRead(); });
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

void TcpSession::resumeReads() { doRead(); }

void TcpSession::doRead() {
  // Two independent reasons not to read: this session is tearing down, or
  // the thread's ingress ring is under pressure. Asio has no "is a read
  // already outstanding" query, so `m_reading` is what makes resumeReads()
  // idempotent.
  if (m_closing || m_flush_then_close || m_reading) return;
  if (m_context.io.readsSuspended()) return;
  if (m_read_size >= m_read_buffer.size()) {
    close("read buffer exhausted");
    return;
  }
  auto self{shared_from_this()};
  m_socket.async_read_some(
      asio::buffer(m_read_buffer.data() + m_read_size,
                   m_read_buffer.size() - m_read_size),
      [this, self](const boost::system::error_code& ec, std::size_t bytes) {
        m_reading = false;
        if (ec) {
          close(ec == asio::error::eof ? "peer closed" : ec.message());
          return;
        }
        onRead(bytes);
      });
  m_reading = true;
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

  // Per-session credit. Rejecting immediately, on this thread, is the point:
  // a client that outruns its own acks is told so without ever reaching the
  // ring, so one runaway loop cannot crowd out its neighbours.
  if (!m_pump->tryReserve()) {
    sendReject(RejectCode::Throttled, command.client_order_id,
               command.order_id);
    return true;
  }
  m_pump->submit(command);
  return true;
}

bool TcpSession::authenticate(std::span<const std::byte> frame,
                              const Command& command) {
  Binary::LogonBody body{};
  if (!Binary::readBody(frame.subspan(Binary::kHeaderSize), body)) {
    close("short logon");
    return false;
  }

  const std::size_t length{::strnlen(body.api_key, sizeof(body.api_key))};
  const AuthOutcome outcome{
      authenticateSession(m_context.traders, m_context.io.limiter(), m_peer_ip,
                          std::string_view{body.api_key, length}, nowNs())};
  if (!outcome.ok) {
    sendReject(outcome.code, 0, 0);
    closeAfterFlush("authentication failed");
    return false;
  }
  m_trader_id = outcome.trader_id;

  // Only the resolved identity crosses the ring; the key never does.
  Command opened{command};
  opened.trader_id = m_trader_id;
  static_cast<void>(m_pump->tryReserve());
  m_pump->submit(opened);

  logLine("session {:#x} logged on as trader {} ({})", m_session_id,
          outcome.trader_id, outcome.name);
  return true;
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
  for (const Event& event : events) {
    append(event);
    // Exactly one event per command carries this, so the count is exact
    // rather than an estimate. See EventFlags::kCommandComplete.
    if ((event.flags & EventFlags::kCommandComplete) != 0) {
      m_pump->onCommandComplete();
    }
  }
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
  m_pump->cancel();
  m_socket.shutdown(tcp::socket::shutdown_both, ec);
  m_socket.close(ec);
  m_context.io.sessions().remove(m_session_id);

  logLine("session {:#x} closed: {}", m_session_id, reason);
}
}  // namespace Exchange::Net
