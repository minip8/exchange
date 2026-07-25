/*
exchange_cli — an interactive client for the binary protocol.

This is what makes the exchange usable a phase before the browser GUI exists:
two people on two terminals can trade against each other through it.

It is also the reference implementation of the client side of the protocol —
in particular of the framing rule (read into a growable buffer, consume every
complete frame, compact the remainder) and of the market-data gap check.

    exchange_cli --key alice-dev-key-change-me
    > book NVDA          create an instrument
    > books              list them
    > buy 1 100 50       book_id quantity price
    > sell 1 100 51
    > sub 1              subscribe to market data
    > orders             show working orders
    > cancel <order_id>
    > amend <order_id> <quantity> <price>
    > help

`--tail <book_id>` runs non-interactively, reconstructing the book from the
snapshot and the deltas that follow it, and prints the ladder on every
update. `--verify` additionally re-requests a snapshot at the end and
compares — a real, automatable market-data correctness check.
*/
#include <algorithm>
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <charconv>
#include <cstring>
#include <iostream>
#include <map>
#include <print>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "net/core/RejectCode.hpp"
#include "net/wire/BinaryProtocol.hpp"
#include "net/wire/MessageNames.hpp"

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using namespace Exchange::Net;
namespace Wire = Exchange::Net::Binary;

namespace {

struct Options {
  std::string host{"127.0.0.1"};
  uint16_t port{9001};
  std::string api_key{};
  bool cancel_on_disconnect{false};
  uint64_t tail_book{0};
  bool tail{false};
  bool verify{false};
  uint32_t tail_seconds{5};
};

bool parseUint(std::string_view text, auto& out) {
  const auto* const end{text.data() + text.size()};
  const auto result{std::from_chars(text.data(), end, out)};
  return result.ec == std::errc{} && result.ptr == end;
}

std::string_view rejectName(uint8_t code) {
  return toString(static_cast<RejectCode>(code));
}

// One side of a reconstructed ladder. Ordered so the best price is first:
// bids descend, offers ascend — the same convention the engine uses.
struct Ladder {
  std::map<uint64_t, uint64_t, std::greater<>> bids{};
  std::map<uint64_t, uint64_t> offers{};

  void apply(uint8_t side, uint64_t price, uint64_t quantity) {
    // quantity 0 is the standard L2 delete. There is no separate message.
    if (side == 0) {
      if (quantity == 0) {
        bids.erase(price);
      } else {
        bids[price] = quantity;
      }
    } else {
      if (quantity == 0) {
        offers.erase(price);
      } else {
        offers[price] = quantity;
      }
    }
  }

  void clear() {
    bids.clear();
    offers.clear();
  }

  bool operator==(const Ladder&) const = default;

  std::string render() const {
    std::ostringstream out{};
    auto bid{bids.begin()};
    auto offer{offers.begin()};
    out << "      bid       |       ask\n";
    while (bid != bids.end() || offer != offers.end()) {
      std::ostringstream left{};
      std::ostringstream right{};
      if (bid != bids.end()) {
        left << bid->second << " @ " << bid->first;
        ++bid;
      }
      if (offer != offers.end()) {
        right << offer->first << " x " << offer->second;
        ++offer;
      }
      out << std::string(16 - std::min<std::size_t>(16, left.str().size()), ' ')
          << left.str() << " | " << right.str() << "\n";
    }
    return out.str();
  }
};

class Client {
 public:
  Client(asio::io_context& io, Options options)
      : m_socket(io), m_options(std::move(options)) {}

  bool connect(std::string& error) {
    boost::system::error_code ec{};
    tcp::resolver resolver{m_socket.get_executor()};
    const auto endpoints{
        resolver.resolve(m_options.host, std::to_string(m_options.port), ec)};
    if (ec) {
      error = "resolve: " + ec.message();
      return false;
    }
    asio::connect(m_socket, endpoints, ec);
    if (ec) {
      error = "connect: " + ec.message();
      return false;
    }
    m_socket.set_option(tcp::no_delay{true}, ec);

    std::vector<std::byte> frame{};
    Wire::encodeLogon(m_options.api_key, m_options.cancel_on_disconnect,
                      ++m_seq, frame);
    return write(frame, error);
  }

  bool write(const std::vector<std::byte>& frame, std::string& error) {
    boost::system::error_code ec{};
    asio::write(m_socket, asio::buffer(frame), ec);
    if (ec) {
      error = "write: " + ec.message();
      return false;
    }
    return true;
  }

  uint32_t nextSeq() { return ++m_seq; }
  tcp::socket& socket() { return m_socket; }

  /*
  Reads whatever is available and dispatches every complete frame.

  Same shape as the server side, and for the same reason: one read syscall
  usually carries several messages, and reading an 8-byte header then a body
  would double the syscall count for no benefit.
  */
  void pump() {
    boost::system::error_code ec{};
    if (m_buffer.size() - m_size < 4096) m_buffer.resize(m_buffer.size() * 2);
    const std::size_t bytes{m_socket.read_some(
        asio::buffer(m_buffer.data() + m_size, m_buffer.size() - m_size), ec)};
    if (ec) {
      if (ec != asio::error::would_block && ec != asio::error::try_again) {
        m_closed = true;
      }
      return;
    }
    m_size += bytes;

    std::size_t offset{0};
    while (m_size - offset >= Wire::kHeaderSize) {
      Wire::Header header{};
      std::memcpy(&header, m_buffer.data() + offset, Wire::kHeaderSize);
      if (header.length < Wire::kHeaderSize ||
          header.length > Wire::kMaxFrameSize) {
        std::println(stderr, "desynchronized: bad frame length {}",
                     header.length);
        m_closed = true;
        return;
      }
      if (m_size - offset < header.length) break;
      onFrame(header, std::span<const std::byte>{
                          m_buffer.data() + offset + Wire::kHeaderSize,
                          header.length - Wire::kHeaderSize});
      offset += header.length;
    }

    if (offset > 0) {
      m_size -= offset;
      if (m_size > 0) {
        std::memmove(m_buffer.data(), m_buffer.data() + offset, m_size);
      }
    }
  }

  bool closed() const noexcept { return m_closed; }
  const Ladder& ladder() const noexcept { return m_ladder; }
  bool sawGap() const noexcept { return m_saw_gap; }
  bool inSnapshot() const noexcept { return m_in_snapshot; }

 private:
  void onFrame(const Wire::Header& header, std::span<const std::byte> body) {
    switch (header.type) {
      case MsgType::LogonAck: {
        Wire::LogonAckBody ack{};
        if (!Wire::readBody(body, ack)) return;
        if (ack.reject_code != 0) {
          std::println("logon rejected: {}", rejectName(ack.reject_code));
          m_closed = true;
          return;
        }
        std::println("logged on: session {:#x}, trader {}, {} book(s)",
                     ack.session_id, ack.trader_id, ack.book_count);
        return;
      }
      case MsgType::Reject: {
        Wire::RejectBody reject{};
        if (!Wire::readBody(body, reject)) return;
        std::println("REJECT coid={} order={} {}", reject.client_order_id,
                     reject.order_id, rejectName(reject.reject_code));
        return;
      }
      case MsgType::ServerHeartbeat:
        return;

      case MsgType::OrderAck:
      case MsgType::CancelAck:
      case MsgType::AmendAck: {
        Wire::AckBody ack{};
        if (!Wire::readBody(body, ack)) return;
        if (header.type == MsgType::OrderAck) {
          m_working[ack.order_id] = ack;
          // AckBody::quantity is `leaves`, not the original order size —
          // an order that filled on entry acks with 0 here.
          std::println("ACK order {} coid={} {} @ {} (leaves {})", ack.order_id,
                       ack.client_order_id, ack.side == 0 ? "buy" : "sell",
                       ack.price, ack.quantity);
        } else if (header.type == MsgType::CancelAck) {
          m_working.erase(ack.order_id);
          std::println("CANCELLED order {} ({} pulled)", ack.order_id,
                       ack.quantity);
        } else {
          m_working.erase(ack.orig_order_id);
          m_working[ack.order_id] = ack;
          // The new id is not a bug: OrderBook::modifyOrder cannot amend, so
          // the server does remove + re-add and mints a fresh id, FIX
          // OrigClOrdID style. Priority is lost even when shrinking.
          std::println("AMENDED order {} -> {} ({} @ {})", ack.orig_order_id,
                       ack.order_id, ack.quantity, ack.price);
        }
        return;
      }

      case MsgType::ExecReport: {
        Wire::ExecBody exec{};
        if (!Wire::readBody(body, exec)) return;
        const bool aggressor{(exec.flags & EventFlags::kAggressor) != 0};
        std::println("FILL exec={} order={} {} {} @ {} leaves {} [{}]",
                     exec.exec_id, exec.order_id,
                     exec.side == 0 ? "buy" : "sell", exec.quantity, exec.price,
                     exec.leaves, aggressor ? "took" : "made");
        if (exec.leaves == 0) m_working.erase(exec.order_id);
        return;
      }

      case MsgType::CreateBookAck:
      case MsgType::BookEntry: {
        Wire::BookBody book{};
        if (!Wire::readBody(body, book)) return;
        if (book.reject_code != 0) {
          std::println("book rejected: {}", rejectName(book.reject_code));
          return;
        }
        const std::size_t length{
            static_cast<std::size_t>(::strnlen(book.symbol, 8))};
        std::println("book {} = {} (scale {})", book.book_id,
                     std::string_view{book.symbol, length}, book.price_scale);
        return;
      }
      case MsgType::BookListEnd: {
        Wire::BookBody book{};
        if (!Wire::readBody(body, book)) return;
        std::println("({} book(s))", book.count);
        return;
      }

      case MsgType::SnapshotBegin: {
        Wire::MdBody md{};
        if (!Wire::readBody(body, md)) return;
        m_in_snapshot = true;
        m_ladder.clear();
        m_md_seq = md.md_seq;
        std::println("--- snapshot book {} seq {} ({} levels, depth {}) ---",
                     md.book_id, md.md_seq, md.aggregate, md.depth);
        return;
      }
      case MsgType::SnapshotEnd: {
        Wire::MdBody md{};
        if (!Wire::readBody(body, md)) return;
        m_in_snapshot = false;
        m_md_seq = md.md_seq;
        std::print("{}", m_ladder.render());
        return;
      }
      case MsgType::LevelUpdate: {
        Wire::MdBody md{};
        if (!Wire::readBody(body, md)) return;
        if (!m_in_snapshot) checkSequence(md.md_seq);
        m_ladder.apply(md.side, md.price, md.quantity);
        if (!m_in_snapshot) std::print("{}", m_ladder.render());
        return;
      }
      case MsgType::TradePrint: {
        Wire::MdBody md{};
        if (!Wire::readBody(body, md)) return;
        checkSequence(md.md_seq);
        std::println("TRADE {} @ {} ({})", md.quantity, md.price,
                     md.side == 0 ? "buy aggressor" : "sell aggressor");
        return;
      }

      case MsgType::PositionUpdate: {
        Wire::PositionBody pos{};
        if (!Wire::readBody(body, pos)) return;
        std::println("POSITION book {} net {} avg {} realized {} unreal {}",
                     pos.book_id, pos.net_quantity, pos.avg_cost,
                     pos.realized_pnl, pos.unrealized_pnl);
        return;
      }

      default:
        std::println("unhandled {}", nameOf(header.type));
        return;
    }
  }

  // The whole client-side market-data contract: every message must be
  // exactly one more than the last. A gap means resync with get_snapshot.
  void checkSequence(uint64_t md_seq) {
    if (m_md_seq != 0 && md_seq != m_md_seq + 1) {
      std::println(stderr, "MD GAP: expected {} got {} — resnapshot needed",
                   m_md_seq + 1, md_seq);
      m_saw_gap = true;
    }
    m_md_seq = md_seq;
  }

  tcp::socket m_socket;
  Options m_options;
  uint32_t m_seq{0};
  std::vector<std::byte> m_buffer = std::vector<std::byte>(64 * 1024);
  std::size_t m_size{0};
  bool m_closed{false};

  std::map<uint64_t, Wire::AckBody> m_working{};
  Ladder m_ladder{};
  uint64_t m_md_seq{0};
  bool m_in_snapshot{false};
  bool m_saw_gap{false};
};

void printHelp() {
  std::println(
      "commands:\n"
      "  book <SYMBOL> [scale]        create an instrument\n"
      "  books                        list instruments\n"
      "  buy  <book> <qty> <price>    limit buy   (price 0 = market)\n"
      "  sell <book> <qty> <price>    limit sell\n"
      "  ioc  <buy|sell> <book> <qty> <price>\n"
      "  cancel <order_id>\n"
      "  amend  <order_id> <qty> <price>\n"
      "  sub <book> | unsub <book> | snap <book>\n"
      "  help | quit");
}

// Returns false to quit.
bool runCommand(Client& client, const std::string& line, uint64_t& next_coid) {
  std::istringstream input{line};
  std::string verb{};
  input >> verb;
  if (verb.empty()) return true;

  std::vector<std::byte> frame{};
  std::string error{};

  auto send{[&] {
    if (!client.write(frame, error)) std::println(stderr, "{}", error);
  }};

  if (verb == "quit" || verb == "exit") return false;
  if (verb == "help") {
    printHelp();
    return true;
  }

  if (verb == "book") {
    std::string symbol{};
    uint32_t scale{2};
    input >> symbol;
    if (!(input >> scale)) scale = 2;
    Wire::CreateBookBody body{.symbol = {}, .price_scale = scale, .pad = {}};
    std::memcpy(body.symbol, symbol.data(),
                std::min<std::size_t>(8, symbol.size()));
    Wire::encodeCreateBook(body, client.nextSeq(), frame);
    send();
    return true;
  }

  if (verb == "books") {
    Wire::encodeSimple(MsgType::ListBooks, client.nextSeq(), frame);
    send();
    return true;
  }

  if (verb == "buy" || verb == "sell" || verb == "ioc") {
    std::string side_word{verb};
    if (verb == "ioc") input >> side_word;
    uint64_t book{0};
    uint64_t quantity{0};
    uint64_t price{0};
    input >> book >> quantity >> price;
    Wire::NewOrderBody body{
        .client_order_id = ++next_coid,
        .book_id = book,
        .price = price,
        .quantity = quantity,
        .side = static_cast<uint8_t>(side_word == "buy" ? 0 : 1),
        .tif = static_cast<uint8_t>(verb == "ioc" ? 1 : 0),
        // Price 0 means "market"; the gateway synthesizes a marketable limit
        // and forces IOC, so the residual is pulled rather than resting at an
        // absurd price.
        .flags = static_cast<uint8_t>(price == 0 ? 1 : 0),
        .pad = {}};
    Wire::encodeNewOrder(body, client.nextSeq(), frame);
    send();
    return true;
  }

  if (verb == "cancel") {
    uint64_t order_id{0};
    input >> order_id;
    Wire::encodeCancel(
        Wire::CancelBody{.client_order_id = 0, .order_id = order_id},
        client.nextSeq(), frame);
    send();
    return true;
  }

  if (verb == "amend") {
    uint64_t order_id{0};
    uint64_t quantity{0};
    uint64_t price{0};
    input >> order_id >> quantity >> price;
    Wire::encodeAmend(Wire::AmendBody{.client_order_id = ++next_coid,
                                      .order_id = order_id,
                                      .price = price,
                                      .quantity = quantity},
                      client.nextSeq(), frame);
    send();
    return true;
  }

  if (verb == "sub" || verb == "unsub" || verb == "snap") {
    uint64_t book{0};
    input >> book;
    const MsgType type{verb == "sub"     ? MsgType::SubscribeMd
                       : verb == "unsub" ? MsgType::UnsubscribeMd
                                         : MsgType::GetSnapshot};
    Wire::encodeSubscribe(
        type, Wire::SubscribeBody{.book_id = book, .depth = 10, .pad = {}},
        client.nextSeq(), frame);
    send();
    return true;
  }

  std::println("unknown command '{}' — try help", verb);
  return true;
}

bool parseArgs(int argc, char** argv, Options& options) {
  for (int i{1}; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    auto next{[&](std::string_view& out) {
      if (i + 1 >= argc) return false;
      out = std::string_view{argv[++i]};
      return true;
    }};
    std::string_view value{};

    if (arg == "--host") {
      if (!next(value)) return false;
      options.host = std::string{value};
    } else if (arg == "--port") {
      if (!next(value) || !parseUint(value, options.port)) return false;
    } else if (arg == "--key") {
      if (!next(value)) return false;
      options.api_key = std::string{value};
    } else if (arg == "--cancel-on-disconnect") {
      options.cancel_on_disconnect = true;
    } else if (arg == "--tail") {
      if (!next(value) || !parseUint(value, options.tail_book)) return false;
      options.tail = true;
    } else if (arg == "--seconds") {
      if (!next(value) || !parseUint(value, options.tail_seconds)) return false;
    } else if (arg == "--verify") {
      options.verify = true;
    } else if (arg == "--help" || arg == "-h") {
      std::println(
          "usage: exchange_cli --key KEY [--host H] [--port N]\n"
          "                    [--cancel-on-disconnect]\n"
          "                    [--tail BOOK_ID [--seconds N] [--verify]]");
      std::exit(0);
    } else {
      std::println(stderr, "unknown argument: {}", arg);
      return false;
    }
  }
  return !options.api_key.empty();
}

/*
Non-interactive market-data check.

Subscribes, follows the deltas for a while, then asks for a fresh snapshot
and compares the ladder it reconstructed against the one the server just
restated. Agreement means the delta stream is complete and correctly ordered
— which is the actual claim being made about market data, and it is
automatable, unlike eyeballing a ladder.
*/
int runTail(Client& client, const Options& options) {
  std::string error{};
  std::vector<std::byte> frame{};
  Wire::encodeSubscribe(
      MsgType::SubscribeMd,
      Wire::SubscribeBody{.book_id = options.tail_book, .depth = 10, .pad = {}},
      client.nextSeq(), frame);
  if (!client.write(frame, error)) {
    std::println(stderr, "{}", error);
    return 1;
  }

  const auto deadline{std::chrono::steady_clock::now() +
                      std::chrono::seconds{options.tail_seconds}};
  while (!client.closed() && std::chrono::steady_clock::now() < deadline) {
    client.pump();
  }
  if (client.sawGap()) {
    std::println(stderr, "FAIL: market data had a sequence gap");
    return 1;
  }
  if (!options.verify) return 0;

  const Ladder from_deltas{client.ladder()};

  frame.clear();
  Wire::encodeSubscribe(
      MsgType::GetSnapshot,
      Wire::SubscribeBody{.book_id = options.tail_book, .depth = 10, .pad = {}},
      client.nextSeq(), frame);
  if (!client.write(frame, error)) {
    std::println(stderr, "{}", error);
    return 1;
  }

  const auto verify_deadline{std::chrono::steady_clock::now() +
                             std::chrono::seconds{2}};
  while (!client.closed() &&
         std::chrono::steady_clock::now() < verify_deadline) {
    client.pump();
    if (!client.inSnapshot() && client.ladder() != Ladder{}) break;
  }

  if (client.ladder() == from_deltas) {
    std::println("OK: the delta-reconstructed book matches a fresh snapshot");
    return 0;
  }
  std::println(stderr,
               "FAIL: delta-reconstructed book differs from the snapshot");
  std::println(stderr, "from deltas:\n{}", from_deltas.render());
  std::println(stderr, "from snapshot:\n{}", client.ladder().render());
  return 1;
}
}  // namespace

int main(int argc, char** argv) {
  Options options{};
  if (!parseArgs(argc, argv, options)) {
    std::println(stderr, "exchange_cli requires --key; see --help");
    return 2;
  }

  asio::io_context io{1};
  Client client{io, options};
  std::string error{};
  if (!client.connect(error)) {
    std::println(stderr, "{}", error);
    return 1;
  }

  if (options.tail) {
    client.socket().non_blocking(true);
    return runTail(client, options);
  }

  // Interactive: one thread pumps the socket, the main thread reads stdin.
  // Crude, and entirely adequate for a diagnostic tool.
  std::thread reader{[&] {
    while (!client.closed()) {
      client.pump();
      std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
  }};
  client.socket().non_blocking(true);

  printHelp();
  uint64_t next_coid{0};
  std::string line{};
  while (!client.closed() && std::getline(std::cin, line)) {
    if (!runCommand(client, line, next_coid)) break;
    // Give the responses a moment to arrive before the next prompt, so the
    // output is not interleaved mid-command.
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
  }

  boost::system::error_code ec{};
  client.socket().shutdown(tcp::socket::shutdown_both, ec);
  client.socket().close(ec);
  reader.join();
  return 0;
}
