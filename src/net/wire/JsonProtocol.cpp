#include "net/wire/JsonProtocol.hpp"

#include <boost/json.hpp>
#include <cstring>

#include "net/core/RejectCode.hpp"
#include "net/core/Side.hpp"
#include "types/Symbol.hpp"

namespace Exchange::Net::Json {
namespace json = boost::json;
using Exchange::Types::Symbol;

namespace {
// Missing fields read as 0 rather than failing. A browser client that omits
// an optional field gets the documented default; a field that matters
// (quantity, price, book_id) is validated by the gateway anyway, which is the
// one place those rules should live.
uint64_t u64(const json::object& object, std::string_view key) {
  const json::value* value{object.if_contains(key)};
  if (value == nullptr) return 0;
  if (value->is_int64()) return static_cast<uint64_t>(value->get_int64());
  if (value->is_uint64()) return value->get_uint64();
  if (value->is_double()) return static_cast<uint64_t>(value->get_double());
  return 0;
}

uint32_t u32(const json::object& object, std::string_view key) {
  return static_cast<uint32_t>(u64(object, key));
}

int64_t i64(const json::object& object, std::string_view key) {
  const json::value* value{object.if_contains(key)};
  if (value == nullptr) return 0;
  if (value->is_int64()) return value->get_int64();
  if (value->is_uint64()) return static_cast<int64_t>(value->get_uint64());
  if (value->is_double()) return static_cast<int64_t>(value->get_double());
  return 0;
}

std::string str(const json::object& object, std::string_view key) {
  const json::value* value{object.if_contains(key)};
  if (value == nullptr || !value->is_string()) return {};
  return std::string{value->get_string().c_str(), value->get_string().size()};
}

Side sideOf(const json::object& object) {
  return u64(object, "side") == 0 ? Side::Buy : Side::Sell;
}

// The envelope every outbound message shares. `type` comes from the same
// table the binary codec uses.
json::object envelope(MsgType type) {
  json::object out{};
  out["type"] = std::string{nameOf(type)};
  return out;
}

std::string toText(const json::object& object) {
  // Qualified, and not named `serialize`: an unqualified call would be
  // ambiguous against boost::json::serialize found by ADL.
  return json::serialize(json::value(object));
}
}  // namespace

JsonResult decode(std::string_view text, uint32_t session_id,
                  uint32_t trader_id, uint64_t recv_ts_ns) {
  JsonResult result{};

  json::error_code ec{};
  // Not brace-initialized: json::value's initializer_list constructor would
  // wrap the parsed document in a one-element array. See TraderDirectory.cpp,
  // where that cost an afternoon.
  const json::value root = json::parse(text, ec);
  if (ec || !root.is_object()) return result;

  const json::object& object{root.as_object()};
  const std::string type_name{str(object, "type")};
  if (type_name.empty()) return result;

  const auto type{typeOf(type_name)};
  if (!type.has_value() || isServerMessage(*type)) {
    // Naming a server-to-client type is the JSON equivalent of the binary
    // protocol's high-bit check: the client is confused about direction.
    result.status = JsonStatus::UnknownType;
    return result;
  }

  result.type = *type;
  Command& command{result.command};
  command.session_id = session_id;
  command.trader_id = trader_id;
  command.recv_ts_ns = recv_ts_ns;

  switch (*type) {
    case MsgType::Logon:
      command.type = CommandType::SessionOpened;
      result.api_key = str(object, "api_key");
      if (u64(object, "cancel_on_disconnect") != 0) {
        command.flags |= CommandFlags::kCancelOnDisconnect;
      }
      break;

    case MsgType::Logout:
      command.type = CommandType::SessionClosed;
      break;

    case MsgType::Heartbeat:
      command.type = CommandType::None;
      break;

    case MsgType::NewOrder:
      command.type = CommandType::NewOrder;
      command.client_order_id = u64(object, "client_order_id");
      command.book_id = u64(object, "book_id");
      command.price = u64(object, "price");
      command.quantity = u64(object, "quantity");
      command.side = sideOf(object);
      command.tif =
          u64(object, "tif") == 0 ? TimeInForce::Gtc : TimeInForce::Ioc;
      if ((u32(object, "flags") & CommandFlags::kMarket) != 0) {
        command.flags |= CommandFlags::kMarket;
      }
      break;

    case MsgType::Cancel:
      command.type = CommandType::Cancel;
      command.client_order_id = u64(object, "client_order_id");
      command.order_id = u64(object, "order_id");
      break;

    case MsgType::Amend:
      command.type = CommandType::Amend;
      command.client_order_id = u64(object, "client_order_id");
      command.order_id = u64(object, "order_id");
      command.price = u64(object, "price");
      command.quantity = u64(object, "quantity");
      break;

    case MsgType::CreateBook: {
      command.type = CommandType::CreateBook;
      // tryMake, always: Symbol's explicit constructor writes past the end
      // for anything over 8 characters, and this string came off the
      // internet.
      const auto symbol{Symbol::tryMake(str(object, "symbol"))};
      if (!symbol.has_value()) return result;  // stays Malformed
      command.symbol = *symbol;
      command.aux = u32(object, "price_scale");
      break;
    }

    case MsgType::ListBooks:
      command.type = CommandType::ListBooks;
      break;

    case MsgType::SubscribeMd:
    case MsgType::UnsubscribeMd:
    case MsgType::GetSnapshot:
      command.type = *type == MsgType::SubscribeMd ? CommandType::SubscribeMd
                     : *type == MsgType::UnsubscribeMd
                         ? CommandType::UnsubscribeMd
                         : CommandType::GetSnapshot;
      command.book_id = u64(object, "book_id");
      command.aux = u32(object, "depth");
      break;

    default:
      result.status = JsonStatus::UnknownType;
      return result;
  }

  result.status = JsonStatus::Ok;
  return result;
}

std::string encode(const Event& event) {
  switch (event.type) {
    case EventType::LogonAck: {
      json::object out{envelope(MsgType::LogonAck)};
      out["session_id"] = event.session_id;
      out["trader_id"] = event.trader_id;
      out["book_count"] = event.payload.ack.quantity;
      out["reject_code"] = static_cast<uint64_t>(event.payload.ack.reject_code);
      out["reject"] = std::string{toString(event.payload.ack.reject_code)};
      return toText(out);
    }

    case EventType::Reject: {
      json::object out{envelope(MsgType::Reject)};
      out["client_order_id"] = event.payload.ack.client_order_id;
      out["order_id"] = event.payload.ack.order_id;
      out["reject_code"] = static_cast<uint64_t>(event.payload.ack.reject_code);
      // The binary protocol makes the client look the code up; the browser
      // gets the name too, so the GUI needs no table of its own.
      out["reject"] = std::string{toString(event.payload.ack.reject_code)};
      return toText(out);
    }

    case EventType::OrderAck:
    case EventType::CancelAck:
    case EventType::AmendAck: {
      const MsgType type{event.type == EventType::OrderAck ? MsgType::OrderAck
                         : event.type == EventType::CancelAck
                             ? MsgType::CancelAck
                             : MsgType::AmendAck};
      json::object out{envelope(type)};
      out["order_id"] = event.payload.ack.order_id;
      out["client_order_id"] = event.payload.ack.client_order_id;
      out["orig_order_id"] = event.payload.ack.orig_order_id;
      out["price"] = event.payload.ack.price;
      out["quantity"] = event.payload.ack.quantity;
      out["book_id"] = event.book_id;
      out["side"] = static_cast<uint64_t>(event.side);
      out["flags"] = event.flags;
      out["reject_code"] = static_cast<uint64_t>(event.payload.ack.reject_code);
      return toText(out);
    }

    case EventType::ExecReport: {
      json::object out{envelope(MsgType::ExecReport)};
      out["exec_id"] = event.payload.exec.exec_id;
      out["order_id"] = event.payload.exec.order_id;
      out["client_order_id"] = event.payload.exec.client_order_id;
      out["price"] = event.payload.exec.price;
      out["quantity"] = event.payload.exec.quantity;
      out["leaves"] = event.payload.exec.leaves;
      out["book_id"] = event.book_id;
      out["side"] = static_cast<uint64_t>(event.side);
      out["flags"] = event.flags;
      return toText(out);
    }

    case EventType::CreateBookAck:
    case EventType::BookEntry:
    case EventType::BookListEnd: {
      const MsgType type{
          event.type == EventType::CreateBookAck ? MsgType::CreateBookAck
          : event.type == EventType::BookEntry   ? MsgType::BookEntry
                                                 : MsgType::BookListEnd};
      json::object out{envelope(type)};
      out["book_id"] = event.payload.book.book_id;
      out["symbol"] = std::string{event.payload.book.symbol.view()};
      out["price_scale"] = event.payload.book.price_scale;
      out["index"] = event.payload.book.index;
      out["count"] = event.payload.book.count;
      out["reject_code"] =
          static_cast<uint64_t>(event.payload.book.reject_code);
      return toText(out);
    }

    case EventType::SnapshotBegin:
    case EventType::LevelUpdate:
    case EventType::SnapshotEnd:
    case EventType::TradePrint: {
      const MsgType type{
          event.type == EventType::SnapshotBegin ? MsgType::SnapshotBegin
          : event.type == EventType::LevelUpdate ? MsgType::LevelUpdate
          : event.type == EventType::SnapshotEnd ? MsgType::SnapshotEnd
                                                 : MsgType::TradePrint};
      json::object out{envelope(type)};
      out["md_seq"] = event.payload.md.md_seq;
      out["price"] = event.payload.md.price;
      out["quantity"] = event.payload.md.quantity;
      out["ts_ns"] = event.payload.md.ts_ns;
      out["aggregate"] = event.payload.md.aggregate;
      out["book_id"] = event.book_id;
      out["depth"] = event.payload.md.depth;
      out["side"] = static_cast<uint64_t>(event.payload.md.level_side);
      out["flags"] = event.flags;
      return toText(out);
    }

    case EventType::PositionUpdate: {
      json::object out{envelope(MsgType::PositionUpdate)};
      out["book_id"] = event.payload.pos.book_id;
      out["net_quantity"] = event.payload.pos.net_quantity;
      out["avg_cost"] = event.payload.pos.avg_cost;
      out["realized_pnl"] = event.payload.pos.realized_pnl;
      out["unrealized_pnl"] = event.payload.pos.unrealized_pnl;
      out["mark_price"] = event.payload.pos.mark_price;
      return toText(out);
    }

    case EventType::None:
      return {};
  }
  return {};
}

std::optional<Event> decodeEvent(std::string_view text) {
  json::error_code ec{};
  const json::value root = json::parse(text, ec);
  if (ec || !root.is_object()) return std::nullopt;
  const json::object& object{root.as_object()};

  const auto type{typeOf(str(object, "type"))};
  if (!type.has_value()) return std::nullopt;

  Event event{};
  event.book_id = u32(object, "book_id");
  event.side = sideOf(object);
  event.flags = static_cast<uint8_t>(u32(object, "flags"));

  auto ack{[&](EventType kind) {
    event.type = kind;
    event.payload.ack = AckPayload{
        .order_id = u64(object, "order_id"),
        .client_order_id = u64(object, "client_order_id"),
        .orig_order_id = u64(object, "orig_order_id"),
        .price = u64(object, "price"),
        .quantity = u64(object, "quantity"),
        .reject_code = static_cast<RejectCode>(u32(object, "reject_code")),
        .pad = {}};
  }};

  auto md{[&](EventType kind) {
    const Side side{sideOf(object)};
    event.type = kind;
    event.side = side;
    event.payload.md = MdPayload{.md_seq = u64(object, "md_seq"),
                                 .price = u64(object, "price"),
                                 .quantity = u64(object, "quantity"),
                                 .ts_ns = u64(object, "ts_ns"),
                                 .aggregate = u64(object, "aggregate"),
                                 .depth = u32(object, "depth"),
                                 .level_side = side,
                                 .pad = {}};
  }};

  auto book{[&](EventType kind) {
    event.type = kind;
    BookPayload payload{
        .symbol = {},
        .book_id = u64(object, "book_id"),
        .price_scale = u32(object, "price_scale"),
        .index = u32(object, "index"),
        .count = u32(object, "count"),
        .reject_code = static_cast<RejectCode>(u32(object, "reject_code")),
        .pad = {}};
    const auto symbol{Symbol::tryMake(str(object, "symbol"))};
    if (symbol.has_value()) payload.symbol = *symbol;
    event.payload.book = payload;
    event.book_id = static_cast<uint32_t>(payload.book_id);
  }};

  switch (*type) {
    case MsgType::LogonAck:
      event.type = EventType::LogonAck;
      event.session_id = u32(object, "session_id");
      event.trader_id = u32(object, "trader_id");
      event.payload.ack = AckPayload{
          .order_id = 0,
          .client_order_id = 0,
          .orig_order_id = 0,
          .price = 0,
          .quantity = u64(object, "book_count"),
          .reject_code = static_cast<RejectCode>(u32(object, "reject_code")),
          .pad = {}};
      break;
    case MsgType::Reject:
      ack(EventType::Reject);
      event.payload.ack.price = 0;
      event.payload.ack.quantity = 0;
      break;
    case MsgType::OrderAck:
      ack(EventType::OrderAck);
      break;
    case MsgType::CancelAck:
      ack(EventType::CancelAck);
      break;
    case MsgType::AmendAck:
      ack(EventType::AmendAck);
      break;

    case MsgType::ExecReport:
      event.type = EventType::ExecReport;
      event.payload.exec =
          ExecPayload{.exec_id = u64(object, "exec_id"),
                      .order_id = u64(object, "order_id"),
                      .client_order_id = u64(object, "client_order_id"),
                      .price = u64(object, "price"),
                      .quantity = u64(object, "quantity"),
                      .leaves = u64(object, "leaves")};
      break;

    case MsgType::CreateBookAck:
      book(EventType::CreateBookAck);
      break;
    case MsgType::BookEntry:
      book(EventType::BookEntry);
      break;
    case MsgType::BookListEnd:
      book(EventType::BookListEnd);
      break;

    case MsgType::SnapshotBegin:
      md(EventType::SnapshotBegin);
      break;
    case MsgType::LevelUpdate:
      md(EventType::LevelUpdate);
      break;
    case MsgType::SnapshotEnd:
      md(EventType::SnapshotEnd);
      break;
    case MsgType::TradePrint:
      md(EventType::TradePrint);
      break;

    case MsgType::PositionUpdate:
      event.type = EventType::PositionUpdate;
      event.payload.pos =
          PosPayload{.book_id = u64(object, "book_id"),
                     .net_quantity = i64(object, "net_quantity"),
                     .avg_cost = i64(object, "avg_cost"),
                     .realized_pnl = i64(object, "realized_pnl"),
                     .unrealized_pnl = i64(object, "unrealized_pnl"),
                     .mark_price = i64(object, "mark_price")};
      break;

    default:
      return std::nullopt;
  }
  return event;
}
}  // namespace Exchange::Net::Json
