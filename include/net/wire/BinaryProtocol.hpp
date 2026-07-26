#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

#include "net/core/Command.hpp"
#include "net/core/Event.hpp"
#include "net/wire/MessageNames.hpp"

namespace Exchange::Net::Binary {
/*
The custom binary protocol for algo clients.

Rules, all of which the structs below are built to satisfy:

  - Little-endian, fixed-size bodies. No varints, no optional fields.
  - Fields are ordered largest-first, so natural alignment produces zero
    padding, and every struct carries a static_assert pinning its size. A
    struct that grows a hole fails the build rather than the wire.
  - Decoded by memcpy, NEVER by reinterpret_cast. A frame boundary in a
    stream buffer has no alignment guarantee, so a cast would be UB — and
    UBSan is on in the Debug tree, so it would be caught, loudly, at the
    worst possible moment.
*/

// The whole protocol assumes the wire order is the host order. Everything
// this runs on is little-endian; the assert is here so a big-endian port
// fails at compile time rather than by scrambling prices.
static_assert(std::endian::native == std::endian::little,
              "the binary protocol is little-endian");

inline constexpr uint8_t kProtocolVersion{1};

/*
`length` counts the header too, so a frame is self-delimiting from its first
two bytes. It is validated against [kHeaderSize, kMaxFrameSize] before being
trusted — the one number an attacker fully controls.
*/
struct Header {
  uint16_t length;
  MsgType type;
  uint8_t version;
  uint32_t seq;
};
static_assert(sizeof(Header) == 8);
static_assert(alignof(Header) <= 4);

inline constexpr std::size_t kHeaderSize{sizeof(Header)};
// Every body below is far smaller than this. The bound exists so a bogus
// length cannot make the server allocate.
inline constexpr std::size_t kMaxFrameSize{1024};

// ---------------------------------------------------------------------------
// Client -> server bodies
// ---------------------------------------------------------------------------

inline constexpr std::size_t kApiKeySize{32};

struct LogonBody {
  char api_key[kApiKeySize];
  uint8_t flags;  // bit 0: cancel_on_disconnect
  uint8_t pad[7];
};
static_assert(sizeof(LogonBody) == 40);

struct NewOrderBody {
  uint64_t client_order_id;
  uint64_t book_id;
  uint64_t price;
  uint64_t quantity;
  uint8_t side;  // 0 buy, 1 sell — see net/core/Side.hpp
  uint8_t tif;   // 0 GTC, 1 IOC
  uint8_t flags;
  uint8_t pad[5];
};
static_assert(sizeof(NewOrderBody) == 40);

struct CancelBody {
  uint64_t client_order_id;
  uint64_t order_id;  // 0 means "resolve through client_order_id"
};
static_assert(sizeof(CancelBody) == 16);

struct AmendBody {
  uint64_t client_order_id;
  uint64_t order_id;
  uint64_t price;
  uint64_t quantity;
};
static_assert(sizeof(AmendBody) == 32);

struct CreateBookBody {
  char symbol[8];
  uint32_t price_scale;
  uint8_t pad[4];
};
static_assert(sizeof(CreateBookBody) == 16);

struct SubscribeBody {
  uint64_t book_id;
  uint32_t depth;  // advisory: the book's own depth wins. See the publisher.
  uint8_t pad[4];
};
static_assert(sizeof(SubscribeBody) == 16);

// ---------------------------------------------------------------------------
// Server -> client bodies
// ---------------------------------------------------------------------------

struct LogonAckBody {
  uint32_t session_id;
  uint32_t trader_id;
  uint32_t book_count;
  uint8_t reject_code;
  uint8_t pad[3];
};
static_assert(sizeof(LogonAckBody) == 16);

struct RejectBody {
  uint64_t client_order_id;
  uint64_t order_id;
  uint8_t reject_code;
  uint8_t pad[7];
};
static_assert(sizeof(RejectBody) == 24);

// OrderAck, CancelAck and AmendAck share one shape: the client needs the same
// fields from all three, and one struct is one place to keep them consistent.
struct AckBody {
  uint64_t order_id;
  uint64_t client_order_id;
  uint64_t orig_order_id;  // amend only: the id being replaced
  uint64_t price;
  uint64_t quantity;  // leaves, or the pulled remainder on a cancel
  uint32_t book_id;
  uint8_t side;
  uint8_t flags;
  uint8_t reject_code;
  uint8_t pad[1];
};
static_assert(sizeof(AckBody) == 48);

struct ExecBody {
  uint64_t exec_id;
  uint64_t order_id;
  uint64_t client_order_id;
  uint64_t price;
  uint64_t quantity;
  uint64_t leaves;
  uint32_t book_id;
  uint8_t side;
  uint8_t flags;
  uint8_t pad[2];
};
static_assert(sizeof(ExecBody) == 56);

// SnapshotBegin / LevelUpdate / SnapshotEnd / TradePrint.
struct MdBody {
  uint64_t md_seq;
  uint64_t price;
  uint64_t quantity;  // 0 on a LevelUpdate means the level is gone
  uint64_t ts_ns;
  uint64_t aggregate;  // SnapshotBegin/End: level count
  uint32_t book_id;
  uint32_t depth;
  uint8_t side;
  uint8_t flags;
  uint8_t pad[6];
};
static_assert(sizeof(MdBody) == 56);

struct BookBody {
  uint64_t book_id;
  char symbol[8];
  uint32_t price_scale;
  uint32_t index;
  uint32_t count;
  uint8_t reject_code;
  uint8_t pad[3];
};
static_assert(sizeof(BookBody) == 32);

struct PositionBody {
  uint64_t book_id;
  int64_t net_quantity;
  int64_t avg_cost;
  int64_t realized_pnl;
  int64_t unrealized_pnl;
  int64_t mark_price;
};
static_assert(sizeof(PositionBody) == 48);

// The largest body, so a frame buffer can be sized once.
inline constexpr std::size_t kMaxBodySize{sizeof(ExecBody)};
static_assert(kHeaderSize + kMaxBodySize <= kMaxFrameSize);

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

// The only way a body is ever read out of a buffer. See the note above about
// alignment: `bytes.data()` points into a stream buffer at an arbitrary
// offset, and reinterpret_cast'ing it would be UB.
template <typename Body>
  requires std::is_trivially_copyable_v<Body>
bool readBody(std::span<const std::byte> bytes, Body& out) noexcept {
  if (bytes.size() < sizeof(Body)) return false;
  std::memcpy(&out, bytes.data(), sizeof(Body));
  return true;
}

inline bool readHeader(std::span<const std::byte> bytes, Header& out) noexcept {
  if (bytes.size() < kHeaderSize) return false;
  std::memcpy(&out, bytes.data(), kHeaderSize);
  return true;
}

enum class DecodeStatus : uint8_t {
  Ok,
  NeedMore,     // not a whole frame yet; keep reading
  Malformed,    // bad length or version; the session must be torn down
  UnknownType,  // well-formed but unroutable; reject and carry on
};

struct DecodeResult {
  DecodeStatus status{DecodeStatus::NeedMore};
  std::size_t consumed{0};
  Command command{};
  MsgType type{MsgType::None};
  uint32_t seq{0};
};

/*
Decodes at most one frame from the front of `bytes`.

The caller loops this over its receive buffer, consuming every complete frame
before compacting the remainder — see TcpSession::parseFrames. Deliberately
NOT async_read(8) followed by async_read(n): that is two syscalls per message
and would dominate any latency number this project produces.

`session_id`, `trader_id` and `recv_ts_ns` come from the session, not the
wire — a client cannot assert its own identity or its own receive timestamp.
*/
DecodeResult decode(std::span<const std::byte> bytes, uint32_t session_id,
                    uint32_t trader_id, uint64_t recv_ts_ns) noexcept;

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

/*
Decodes one server-to-client frame back into an Event.

Exists for two reasons: exchange_cli needs it, and net_smoke needs it to
round-trip every message through the codec and compare. A codec that can only
encode is a codec whose bugs are found by a human reading hex.

Not every Event field survives the trip — session_id and trader_id are the
server's own routing and are not on the wire (except in LogonAck, which is
where the client learns them). See net_smoke's normalizeForCompare.
*/
std::optional<Event> decodeEvent(MsgType type,
                                 std::span<const std::byte> body) noexcept;

// Appends one framed message for `event` to `out`. Returns false for events
// that have no wire representation (there are none today, but the codec
// should fail visibly rather than emit a truncated frame if one appears).
bool encode(const Event& event, uint32_t seq, std::vector<std::byte>& out);

// Client-side encoders, used by exchange_cli. Kept here rather than in the
// tool so that both directions of the protocol live in one file.
void encodeLogon(std::string_view api_key, bool cancel_on_disconnect,
                 uint32_t seq, std::vector<std::byte>& out);
void encodeNewOrder(const NewOrderBody& body, uint32_t seq,
                    std::vector<std::byte>& out);
void encodeCancel(const CancelBody& body, uint32_t seq,
                  std::vector<std::byte>& out);
void encodeAmend(const AmendBody& body, uint32_t seq,
                 std::vector<std::byte>& out);
void encodeCreateBook(const CreateBookBody& body, uint32_t seq,
                      std::vector<std::byte>& out);
void encodeSimple(MsgType type, uint32_t seq, std::vector<std::byte>& out);
void encodeSubscribe(MsgType type, const SubscribeBody& body, uint32_t seq,
                     std::vector<std::byte>& out);
}  // namespace Exchange::Net::Binary
