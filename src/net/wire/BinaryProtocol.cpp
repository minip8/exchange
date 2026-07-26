#include "net/wire/BinaryProtocol.hpp"

#include <algorithm>

#include "net/core/Side.hpp"
#include "types/Symbol.hpp"

namespace Exchange::Net::Binary {
namespace {
using Exchange::Types::Symbol;

template <typename Body>
void append(MsgType type, uint32_t seq, const Body& body,
            std::vector<std::byte>& out) {
  const Header header{
      .length = static_cast<uint16_t>(kHeaderSize + sizeof(Body)),
      .type = type,
      .version = kProtocolVersion,
      .seq = seq};
  const std::size_t offset{out.size()};
  out.resize(offset + kHeaderSize + sizeof(Body));
  std::memcpy(out.data() + offset, &header, kHeaderSize);
  std::memcpy(out.data() + offset + kHeaderSize, &body, sizeof(Body));
}

void appendHeaderOnly(MsgType type, uint32_t seq, std::vector<std::byte>& out) {
  const Header header{.length = static_cast<uint16_t>(kHeaderSize),
                      .type = type,
                      .version = kProtocolVersion,
                      .seq = seq};
  const std::size_t offset{out.size()};
  out.resize(offset + kHeaderSize);
  std::memcpy(out.data() + offset, &header, kHeaderSize);
}

Side sideFrom(uint8_t byte) noexcept {
  return byte == 0 ? Side::Buy : Side::Sell;
}

TimeInForce tifFrom(uint8_t byte) noexcept {
  return byte == 0 ? TimeInForce::Gtc : TimeInForce::Ioc;
}

DecodeResult malformed(std::size_t consumed) {
  return DecodeResult{.status = DecodeStatus::Malformed,
                      .consumed = consumed,
                      .command = {},
                      .type = MsgType::None,
                      .seq = 0};
}
}  // namespace

DecodeResult decode(std::span<const std::byte> bytes, uint32_t session_id,
                    uint32_t trader_id, uint64_t recv_ts_ns) noexcept {
  Header header{};
  if (!readHeader(bytes, header)) {
    return DecodeResult{.status = DecodeStatus::NeedMore};
  }

  // Validate the length before trusting it for anything. This is the single
  // field an attacker fully controls, and everything downstream indexes with
  // it.
  if (header.length < kHeaderSize || header.length > kMaxFrameSize) {
    return malformed(0);
  }
  if (header.version != kProtocolVersion) return malformed(0);
  // A client sending a server-side type means it has lost sync (or is
  // confused about which end it is). Either way the stream is untrustworthy.
  if (isServerMessage(header.type)) return malformed(0);

  if (bytes.size() < header.length) {
    return DecodeResult{.status = DecodeStatus::NeedMore};
  }

  const std::span<const std::byte> body{bytes.subspan(
      kHeaderSize, static_cast<std::size_t>(header.length) - kHeaderSize)};

  DecodeResult result{};
  result.consumed = header.length;
  result.type = header.type;
  result.seq = header.seq;

  Command& command{result.command};
  command.session_id = session_id;
  command.trader_id = trader_id;
  command.recv_ts_ns = recv_ts_ns;

  auto ok{[&] {
    result.status = DecodeStatus::Ok;
    return result;
  }};

  switch (header.type) {
    case MsgType::Logon: {
      LogonBody logon{};
      if (!readBody(body, logon)) return malformed(result.consumed);
      command.type = CommandType::SessionOpened;
      if ((logon.flags & 0x01u) != 0) {
        command.flags |= CommandFlags::kCancelOnDisconnect;
      }
      // The key itself is not carried in the Command: authentication happens
      // on the I/O thread, against an immutable map, and only the resolved
      // trader_id crosses the ring. Secrets never enter the matching thread.
      return ok();
    }

    case MsgType::Logout: {
      command.type = CommandType::SessionClosed;
      return ok();
    }

    case MsgType::Heartbeat: {
      // Answered on the I/O thread; nothing for the matching thread to do.
      command.type = CommandType::None;
      return ok();
    }

    case MsgType::NewOrder: {
      NewOrderBody order{};
      if (!readBody(body, order)) return malformed(result.consumed);
      command.type = CommandType::NewOrder;
      command.client_order_id = order.client_order_id;
      command.book_id = order.book_id;
      command.price = order.price;
      command.quantity = order.quantity;
      command.side = sideFrom(order.side);
      command.tif = tifFrom(order.tif);
      if ((order.flags & 0x01u) != 0) command.flags |= CommandFlags::kMarket;
      return ok();
    }

    case MsgType::Cancel: {
      CancelBody cancel{};
      if (!readBody(body, cancel)) return malformed(result.consumed);
      command.type = CommandType::Cancel;
      command.client_order_id = cancel.client_order_id;
      command.order_id = cancel.order_id;
      return ok();
    }

    case MsgType::Amend: {
      AmendBody amend{};
      if (!readBody(body, amend)) return malformed(result.consumed);
      command.type = CommandType::Amend;
      command.client_order_id = amend.client_order_id;
      command.order_id = amend.order_id;
      command.price = amend.price;
      command.quantity = amend.quantity;
      return ok();
    }

    case MsgType::CreateBook: {
      CreateBookBody create{};
      if (!readBody(body, create)) return malformed(result.consumed);
      command.type = CommandType::CreateBook;
      // Symbol's explicit string_view constructor writes past the end for
      // anything longer than 8 characters — UB on untrusted input — so the
      // wire path goes through tryMake, always. The field is a fixed 8 bytes,
      // so the length is the run up to the first NUL.
      const std::size_t length{static_cast<std::size_t>(
          std::find(create.symbol, create.symbol + sizeof(create.symbol),
                    '\0') -
          create.symbol)};
      const auto symbol{
          Symbol::tryMake(std::string_view{create.symbol, length})};
      if (!symbol.has_value()) return malformed(result.consumed);
      command.symbol = *symbol;
      command.aux = create.price_scale;
      return ok();
    }

    case MsgType::ListBooks: {
      command.type = CommandType::ListBooks;
      return ok();
    }

    case MsgType::SubscribeMd:
    case MsgType::UnsubscribeMd:
    case MsgType::GetSnapshot: {
      SubscribeBody subscribe{};
      if (!readBody(body, subscribe)) return malformed(result.consumed);
      command.type = header.type == MsgType::SubscribeMd
                         ? CommandType::SubscribeMd
                         : (header.type == MsgType::UnsubscribeMd
                                ? CommandType::UnsubscribeMd
                                : CommandType::GetSnapshot);
      command.book_id = subscribe.book_id;
      command.aux = subscribe.depth;
      return ok();
    }

    default:
      // Well-formed framing, unroutable content: consume the frame and let
      // the caller reject it. Unlike a bad length, this does not desync the
      // stream, so there is no reason to drop the connection.
      result.status = DecodeStatus::UnknownType;
      return result;
  }
}

std::optional<Event> decodeEvent(MsgType type,
                                 std::span<const std::byte> body) noexcept {
  Event event{};
  event.type = EventType::None;

  auto readAck{[&](EventType kind) -> std::optional<Event> {
    AckBody ack{};
    if (!readBody(body, ack)) return std::nullopt;
    event.type = kind;
    event.book_id = ack.book_id;
    event.side = ack.side == 0 ? Side::Buy : Side::Sell;
    event.flags = ack.flags;
    event.payload.ack =
        AckPayload{.order_id = ack.order_id,
                   .client_order_id = ack.client_order_id,
                   .orig_order_id = ack.orig_order_id,
                   .price = ack.price,
                   .quantity = ack.quantity,
                   .reject_code = static_cast<RejectCode>(ack.reject_code),
                   .pad = {}};
    return event;
  }};

  auto readMd{[&](EventType kind) -> std::optional<Event> {
    MdBody md{};
    if (!readBody(body, md)) return std::nullopt;
    const Side side{md.side == 0 ? Side::Buy : Side::Sell};
    event.type = kind;
    event.book_id = md.book_id;
    event.side = side;
    event.flags = md.flags;
    event.payload.md = MdPayload{.md_seq = md.md_seq,
                                 .price = md.price,
                                 .quantity = md.quantity,
                                 .ts_ns = md.ts_ns,
                                 .aggregate = md.aggregate,
                                 .depth = md.depth,
                                 .level_side = side,
                                 .pad = {}};
    return event;
  }};

  auto readBook{[&](EventType kind) -> std::optional<Event> {
    BookBody book{};
    if (!readBody(body, book)) return std::nullopt;
    event.type = kind;
    event.book_id = static_cast<uint32_t>(book.book_id);
    BookPayload payload{
        .symbol = {},
        .book_id = book.book_id,
        .price_scale = book.price_scale,
        .index = book.index,
        .count = book.count,
        .reject_code = static_cast<RejectCode>(book.reject_code),
        .pad = {}};
    std::memcpy(payload.symbol.value.data(), book.symbol,
                payload.symbol.value.size());
    event.payload.book = payload;
    return event;
  }};

  switch (type) {
    case MsgType::LogonAck: {
      LogonAckBody ack{};
      if (!readBody(body, ack)) return std::nullopt;
      event.type = EventType::LogonAck;
      // The one message that does carry the session's identity: it is where
      // the client learns it.
      event.session_id = ack.session_id;
      event.trader_id = ack.trader_id;
      event.payload.ack =
          AckPayload{.order_id = 0,
                     .client_order_id = 0,
                     .orig_order_id = 0,
                     .price = 0,
                     .quantity = ack.book_count,
                     .reject_code = static_cast<RejectCode>(ack.reject_code),
                     .pad = {}};
      return event;
    }
    case MsgType::Reject: {
      RejectBody reject{};
      if (!readBody(body, reject)) return std::nullopt;
      event.type = EventType::Reject;
      event.payload.ack =
          AckPayload{.order_id = reject.order_id,
                     .client_order_id = reject.client_order_id,
                     .orig_order_id = 0,
                     .price = 0,
                     .quantity = 0,
                     .reject_code = static_cast<RejectCode>(reject.reject_code),
                     .pad = {}};
      return event;
    }
    case MsgType::OrderAck:
      return readAck(EventType::OrderAck);
    case MsgType::CancelAck:
      return readAck(EventType::CancelAck);
    case MsgType::AmendAck:
      return readAck(EventType::AmendAck);

    case MsgType::ExecReport: {
      ExecBody exec{};
      if (!readBody(body, exec)) return std::nullopt;
      event.type = EventType::ExecReport;
      event.book_id = exec.book_id;
      event.side = exec.side == 0 ? Side::Buy : Side::Sell;
      event.flags = exec.flags;
      event.payload.exec = ExecPayload{.exec_id = exec.exec_id,
                                       .order_id = exec.order_id,
                                       .client_order_id = exec.client_order_id,
                                       .price = exec.price,
                                       .quantity = exec.quantity,
                                       .leaves = exec.leaves};
      return event;
    }

    case MsgType::CreateBookAck:
      return readBook(EventType::CreateBookAck);
    case MsgType::BookEntry:
      return readBook(EventType::BookEntry);
    case MsgType::BookListEnd:
      return readBook(EventType::BookListEnd);

    case MsgType::SnapshotBegin:
      return readMd(EventType::SnapshotBegin);
    case MsgType::LevelUpdate:
      return readMd(EventType::LevelUpdate);
    case MsgType::SnapshotEnd:
      return readMd(EventType::SnapshotEnd);
    case MsgType::TradePrint:
      return readMd(EventType::TradePrint);
    case MsgType::MdAck:
      return readMd(EventType::MdAck);

    case MsgType::PositionUpdate: {
      PositionBody pos{};
      if (!readBody(body, pos)) return std::nullopt;
      event.type = EventType::PositionUpdate;
      event.book_id = static_cast<uint32_t>(pos.book_id);
      event.payload.pos = PosPayload{.book_id = pos.book_id,
                                     .net_quantity = pos.net_quantity,
                                     .avg_cost = pos.avg_cost,
                                     .realized_pnl = pos.realized_pnl,
                                     .unrealized_pnl = pos.unrealized_pnl,
                                     .mark_price = pos.mark_price};
      return event;
    }

    default:
      return std::nullopt;
  }
}

bool encode(const Event& event, uint32_t seq, std::vector<std::byte>& out) {
  const uint8_t side{static_cast<uint8_t>(event.side)};

  auto ackBody{[&](const AckPayload& payload) {
    return AckBody{.order_id = payload.order_id,
                   .client_order_id = payload.client_order_id,
                   .orig_order_id = payload.orig_order_id,
                   .price = payload.price,
                   .quantity = payload.quantity,
                   .book_id = event.book_id,
                   .side = side,
                   .flags = event.flags,
                   .reject_code = static_cast<uint8_t>(payload.reject_code),
                   .pad = {}};
  }};

  auto mdBody{[&](MsgType) {
    const MdPayload& payload{event.payload.md};
    return MdBody{.md_seq = payload.md_seq,
                  .price = payload.price,
                  .quantity = payload.quantity,
                  .ts_ns = payload.ts_ns,
                  .aggregate = payload.aggregate,
                  .book_id = event.book_id,
                  .depth = payload.depth,
                  .side = static_cast<uint8_t>(payload.level_side),
                  .flags = event.flags,
                  .pad = {}};
  }};

  auto bookBody{[&] {
    const BookPayload& payload{event.payload.book};
    BookBody body{.book_id = payload.book_id,
                  .symbol = {},
                  .price_scale = payload.price_scale,
                  .index = payload.index,
                  .count = payload.count,
                  .reject_code = static_cast<uint8_t>(payload.reject_code),
                  .pad = {}};
    std::memcpy(body.symbol, payload.symbol.value.data(),
                payload.symbol.value.size());
    return body;
  }};

  switch (event.type) {
    case EventType::LogonAck: {
      const AckPayload& payload{event.payload.ack};
      append(
          MsgType::LogonAck, seq,
          LogonAckBody{.session_id = event.session_id,
                       .trader_id = event.trader_id,
                       .book_count = static_cast<uint32_t>(payload.quantity),
                       .reject_code = static_cast<uint8_t>(payload.reject_code),
                       .pad = {}},
          out);
      return true;
    }
    case EventType::Reject: {
      const AckPayload& payload{event.payload.ack};
      append(
          MsgType::Reject, seq,
          RejectBody{.client_order_id = payload.client_order_id,
                     .order_id = payload.order_id,
                     .reject_code = static_cast<uint8_t>(payload.reject_code),
                     .pad = {}},
          out);
      return true;
    }
    case EventType::OrderAck:
      append(MsgType::OrderAck, seq, ackBody(event.payload.ack), out);
      return true;
    case EventType::CancelAck:
      append(MsgType::CancelAck, seq, ackBody(event.payload.ack), out);
      return true;
    case EventType::AmendAck:
      append(MsgType::AmendAck, seq, ackBody(event.payload.ack), out);
      return true;

    case EventType::ExecReport: {
      const ExecPayload& payload{event.payload.exec};
      append(MsgType::ExecReport, seq,
             ExecBody{.exec_id = payload.exec_id,
                      .order_id = payload.order_id,
                      .client_order_id = payload.client_order_id,
                      .price = payload.price,
                      .quantity = payload.quantity,
                      .leaves = payload.leaves,
                      .book_id = event.book_id,
                      .side = side,
                      .flags = event.flags,
                      .pad = {}},
             out);
      return true;
    }

    case EventType::CreateBookAck:
      append(MsgType::CreateBookAck, seq, bookBody(), out);
      return true;
    case EventType::BookEntry:
      append(MsgType::BookEntry, seq, bookBody(), out);
      return true;
    case EventType::BookListEnd:
      append(MsgType::BookListEnd, seq, bookBody(), out);
      return true;

    case EventType::SnapshotBegin:
      append(MsgType::SnapshotBegin, seq, mdBody(MsgType::SnapshotBegin), out);
      return true;
    case EventType::LevelUpdate:
      append(MsgType::LevelUpdate, seq, mdBody(MsgType::LevelUpdate), out);
      return true;
    case EventType::SnapshotEnd:
      append(MsgType::SnapshotEnd, seq, mdBody(MsgType::SnapshotEnd), out);
      return true;
    case EventType::TradePrint:
      append(MsgType::TradePrint, seq, mdBody(MsgType::TradePrint), out);
      return true;
    case EventType::MdAck:
      append(MsgType::MdAck, seq, mdBody(MsgType::MdAck), out);
      return true;

    case EventType::PositionUpdate: {
      const PosPayload& payload{event.payload.pos};
      append(MsgType::PositionUpdate, seq,
             PositionBody{.book_id = payload.book_id,
                          .net_quantity = payload.net_quantity,
                          .avg_cost = payload.avg_cost,
                          .realized_pnl = payload.realized_pnl,
                          .unrealized_pnl = payload.unrealized_pnl,
                          .mark_price = payload.mark_price},
             out);
      return true;
    }

    case EventType::None:
      return false;
  }
  return false;
}

void encodeLogon(std::string_view api_key, bool cancel_on_disconnect,
                 uint32_t seq, std::vector<std::byte>& out) {
  LogonBody body{.api_key = {},
                 .flags = static_cast<uint8_t>(cancel_on_disconnect ? 1 : 0),
                 .pad = {}};
  std::memcpy(body.api_key, api_key.data(),
              std::min(api_key.size(), kApiKeySize));
  append(MsgType::Logon, seq, body, out);
}

void encodeNewOrder(const NewOrderBody& body, uint32_t seq,
                    std::vector<std::byte>& out) {
  append(MsgType::NewOrder, seq, body, out);
}

void encodeCancel(const CancelBody& body, uint32_t seq,
                  std::vector<std::byte>& out) {
  append(MsgType::Cancel, seq, body, out);
}

void encodeAmend(const AmendBody& body, uint32_t seq,
                 std::vector<std::byte>& out) {
  append(MsgType::Amend, seq, body, out);
}

void encodeCreateBook(const CreateBookBody& body, uint32_t seq,
                      std::vector<std::byte>& out) {
  append(MsgType::CreateBook, seq, body, out);
}

void encodeSimple(MsgType type, uint32_t seq, std::vector<std::byte>& out) {
  appendHeaderOnly(type, seq, out);
}

void encodeSubscribe(MsgType type, const SubscribeBody& body, uint32_t seq,
                     std::vector<std::byte>& out) {
  append(type, seq, body, out);
}
}  // namespace Exchange::Net::Binary
