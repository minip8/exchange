#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "net/core/Command.hpp"
#include "net/core/Event.hpp"
#include "net/core/RejectCode.hpp"
#include "net/gateway/MatchingThread.hpp"
#include "net/io/ClientSession.hpp"
#include "net/io/IoThread.hpp"
#include "net/io/SessionPump.hpp"
#include "net/wire/MessageNames.hpp"

namespace Exchange::Net {
using tcp = asio::ip::tcp;

struct SessionContext {
  IoThread& io;
  IngressRing& ring;
  const TraderDirectory& traders;
};

/*
One connected algo client, speaking the binary protocol.

Framing: async_read_some into a growable buffer, then a parse loop that
consumes every complete frame before compacting the remainder. Deliberately
NOT async_read(header) followed by async_read(body) — that is two syscalls
per message and would dominate every latency number this project produces.

Everything here runs on one thread (see IoThread), so nothing is atomic and
nothing is a strand.
*/
class TcpSession : public ClientSession,
                   public std::enable_shared_from_this<TcpSession> {
 public:
  TcpSession(tcp::socket socket, SessionContext context, uint32_t session_id);

  void start();
  // Called on this session's own I/O thread by IoThread::dispatch.
  void deliver(std::span<const Event> events) override;
  // Tears the connection down now. Anything unwritten is lost, so error
  // paths that owe the client a reject should use closeAfterFlush.
  void close(std::string_view reason) override;

  uint32_t sessionId() const noexcept override { return m_session_id; }
  uint32_t traderId() const noexcept { return m_trader_id; }
  bool loggedOn() const noexcept { return m_trader_id != 0; }

 private:
  void doRead();
  void onRead(std::size_t bytes);
  void doWrite();
  // Writes everything queued, then closes. Used wherever the client is owed
  // a reason for the disconnect.
  void closeAfterFlush(std::string_view reason);

  // Returns false if the connection is being torn down and parsing must stop.
  bool handleFrame(MsgType type, std::span<const std::byte> frame,
                   const Command& command, uint32_t seq);
  bool authenticate(std::span<const std::byte> frame, const Command& command);

  void sendReject(RejectCode code, uint64_t client_order_id, uint64_t order_id);
  void append(const Event& event);

  tcp::socket m_socket;
  SessionContext m_context;
  uint32_t m_session_id;
  uint32_t m_trader_id{0};  // 0 until logon succeeds
  uint32_t m_peer_ip{0};
  uint32_t m_out_seq{0};

  std::vector<std::byte> m_read_buffer;
  std::size_t m_read_size{0};

  // Double-buffered output: one buffer is in flight while the other
  // accumulates, so a burst of events becomes one write rather than one
  // write per event.
  std::vector<std::byte> m_write_front{};
  std::vector<std::byte> m_write_back{};
  bool m_writing{false};

  // Never blocks on a full ring: it queues and suspends reads instead, so
  // TCP backpressure reaches the client. Shared with WebSocketSession, since
  // getting the ordering wrong there would be the same bug twice.
  std::optional<SessionPump> m_pump{};
  bool m_closing{false};
  bool m_flush_then_close{false};
  std::string m_close_reason{};
};
}  // namespace Exchange::Net
