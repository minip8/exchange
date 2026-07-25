#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace Exchange::Net {
/*
Message types, shared by both codecs.

Server-to-client types have the HIGH BIT SET. That is not decoration: it
means a client that has lost frame sync, or a server fed its own output,
mis-parses into an obviously wrong half of the space immediately, instead of
silently interpreting an ExecReport as a NewOrder.
*/
enum class MsgType : uint8_t {
  None = 0x00,

  // ---- client -> server ----
  Logon = 0x01,
  Logout = 0x02,
  Heartbeat = 0x03,

  NewOrder = 0x10,
  Cancel = 0x11,
  Amend = 0x12,

  CreateBook = 0x20,
  ListBooks = 0x21,

  SubscribeMd = 0x30,
  UnsubscribeMd = 0x31,
  GetSnapshot = 0x32,

  // ---- server -> client ----
  LogonAck = 0x81,
  Reject = 0x82,
  ServerHeartbeat = 0x83,

  OrderAck = 0x90,
  ExecReport = 0x91,
  CancelAck = 0x92,
  AmendAck = 0x93,

  CreateBookAck = 0xA0,
  BookEntry = 0xA1,
  BookListEnd = 0xA2,

  SnapshotBegin = 0xB0,
  LevelUpdate = 0xB1,
  SnapshotEnd = 0xB2,
  TradePrint = 0xB3,
  MdAck = 0xB4,

  PositionUpdate = 0xC0,
};

constexpr bool isServerMessage(MsgType type) noexcept {
  return (static_cast<uint8_t>(type) & 0x80u) != 0;
}

struct MessageName {
  MsgType type;
  std::string_view name;
};

/*
The single source of truth for message naming, used by BOTH codecs.

The binary codec uses it for logging and for exchange_cli's command parser;
the JSON codec uses it as the literal value of the "type" field. Having one
table is the anti-drift mechanism — a message added to one protocol and not
the other fails to name itself, and net_smoke's round-trip test then fails.
*/
inline constexpr std::array<MessageName, 22> kMessageNames{{
    {MsgType::Logon, "logon"},
    {MsgType::Logout, "logout"},
    {MsgType::Heartbeat, "heartbeat"},
    {MsgType::NewOrder, "new_order"},
    {MsgType::Cancel, "cancel"},
    {MsgType::Amend, "amend"},
    {MsgType::CreateBook, "create_book"},
    {MsgType::ListBooks, "list_books"},
    {MsgType::SubscribeMd, "subscribe_md"},
    {MsgType::UnsubscribeMd, "unsubscribe_md"},
    {MsgType::GetSnapshot, "get_snapshot"},
    {MsgType::LogonAck, "logon_ack"},
    {MsgType::Reject, "reject"},
    {MsgType::ServerHeartbeat, "server_heartbeat"},
    {MsgType::OrderAck, "order_ack"},
    {MsgType::ExecReport, "exec_report"},
    {MsgType::CancelAck, "cancel_ack"},
    {MsgType::AmendAck, "amend_ack"},
    {MsgType::CreateBookAck, "create_book_ack"},
    {MsgType::BookEntry, "book_entry"},
    {MsgType::BookListEnd, "book_list_end"},
    {MsgType::PositionUpdate, "position_update"},
}};

// Market-data types are named separately only because they share one body
// struct; keeping them in the table above would suggest four distinct shapes.
inline constexpr std::array<MessageName, 5> kMarketDataNames{{
    {MsgType::SnapshotBegin, "snapshot_begin"},
    {MsgType::LevelUpdate, "level_update"},
    {MsgType::SnapshotEnd, "snapshot_end"},
    {MsgType::TradePrint, "trade_print"},
    {MsgType::MdAck, "md_ack"},
}};

constexpr std::string_view nameOf(MsgType type) noexcept {
  for (const MessageName& entry : kMessageNames) {
    if (entry.type == type) return entry.name;
  }
  for (const MessageName& entry : kMarketDataNames) {
    if (entry.type == type) return entry.name;
  }
  return "unknown";
}

constexpr std::optional<MsgType> typeOf(std::string_view name) noexcept {
  for (const MessageName& entry : kMessageNames) {
    if (entry.name == name) return entry.type;
  }
  for (const MessageName& entry : kMarketDataNames) {
    if (entry.name == name) return entry.type;
  }
  return std::nullopt;
}

static_assert(nameOf(MsgType::NewOrder) == "new_order");
static_assert(typeOf("exec_report") == MsgType::ExecReport);
static_assert(!typeOf("nonsense").has_value());
static_assert(isServerMessage(MsgType::ExecReport));
static_assert(!isServerMessage(MsgType::NewOrder));
}  // namespace Exchange::Net
