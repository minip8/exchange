/*
net_workload_bench — the networking layer, measured on the flash1 workload.

The engine has a benchmark that means something: five volatility regimes, two
million real order-lifecycle messages, a correctness hash. The networking layer
had a synthetic alternating buy/sell at one price. This closes that gap by
replaying the *same* flash1 order stream through the gateway and over TCP, so
the network numbers sit on the same axis as the engine numbers.

Two modes:

  gateway  Commands driven straight through MatchingLoop with an inline sink.
           No sockets, no threads. Directly comparable to the flash1 harness's
           M msgs/s, and the difference between the two is the price of the
           gateway's ownership, leaves tracking and market data — which the
           flash1 adapter has none of, and which is exactly what this exists
           to quantify.

  tcp      A real client (or N of them) on a real loopback socket to a real
           server. Open-loop: sends are paced at a target offered rate and the
           latency clock starts at the SCHEDULED send time, so the number
           includes any backlog the driver itself built up. A closed-loop
           measurement cannot show you a knee; this can.

--- What "identical to the flash1 orders" means, precisely ---

Identical: the message stream. The same records in the same order, the same
sides, quantities and prices, the same IOC ratio, the same cancel/modify
lifecycle, the same 2% duplicate cancels and stale modifies, one book, seed 23.
Prices go through the same sign-bit flip the flash1 adapter uses, so a tick
lands on the same OrderPrice on both paths (see Flash1Workload.hpp).

Not identical, by construction:
  - OrderTime comes from MatchingLoop's m_seq, not the workload's seq. Same
    total order, so FIFO priority is preserved; the absolute values differ.
  - Amend mints a new order id. The flash1 adapter's modify is also
    cancel + reinsert, so the book effect matches; the reported ids do not.
  - There is no correctness hash here. Correctness stays with
    scripts/run_flash1.sh audit. This gates on the reject counts instead: the
    generator plants ~2% duplicate cancels and stale modifies, so a run where
    NOTHING is rejected did not really replay the stream, and a run where
    nearly everything is rejected is a fast failure path rather than a result.

RELEASE ONLY. net_io links `engine`, which propagates debug_options, so a
Debug build of this is sanitized and its numbers are meaningless.

    cmake --build --preset net-bench
    ./build/release/bench/net/net_workload_bench --help
*/
#include <algorithm>
#include <boost/asio/io_context.hpp>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <format>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "BenchClient.hpp"
#include "Flash1Workload.hpp"
#include "net/core/Command.hpp"
#include "net/gateway/EventSink.hpp"
#include "net/gateway/MatchingLoop.hpp"
#include "net/io/Server.hpp"
#include "net/io/SessionPump.hpp"
#include "net/wire/BinaryProtocol.hpp"

using namespace Exchange::Net;
using namespace Exchange::Bench;
namespace Wire = Exchange::Net::Binary;

namespace {

constexpr std::string_view kApiKey{"alice-dev-key-change-me"};
constexpr std::string_view kTradersPath{"config/traders.example.json"};
// The flash1 adapter names its single book FLASH1; same instrument, same name.
constexpr char kSymbol[8]{'F', 'L', 'A', 'S', 'H', '1', 0, 0};

/*
How many commands a session keeps outstanding.

SessionPump::kMaxInFlight is the server's credit limit, and exceeding it earns
an immediate Throttled reject on the I/O thread. A throttled command never
reaches the book, so it does not just distort the latency number — it changes
the workload. Half the limit leaves room for the gap between an ack (which is a
command's FIRST event) and kCommandComplete (its LAST), which is the only
window in which the client's count can lag the server's.
*/
constexpr uint32_t kWindow{SessionPump::kMaxInFlight / 2};

// One write syscall should carry a burst when the driver is behind, but not an
// unbounded one — a multi-megabyte write would sit in the kernel while acks
// for its first message pile up unread.
constexpr std::size_t kMaxWriteBytes{32 * 1024};

// ---------------------------------------------------------------------------
// results
// ---------------------------------------------------------------------------

struct GatewayResult {
  double msgs_per_s{};
  uint64_t messages{};
  uint64_t events{};
  uint64_t unknown_order_rejects{};
  uint64_t other_rejects{};
  Percentiles ns{};
};

struct RateResult {
  uint64_t offered{};  // 0 means unpaced: send as fast as credits allow
  std::size_t clients{1};
  double achieved{};
  Percentiles ns{};
  uint64_t sent{};
  uint64_t acked{};
  uint64_t execs{};
  uint64_t unknown_order_rejects{};
  uint64_t throttled{};
  uint64_t other_rejects{};
  uint64_t ioc_residual_acks{};
  uint64_t desync{};
  uint64_t md_drops{};
  uint64_t wakeups{};
  uint64_t events_drained{};
  bool disconnected{};
  std::string error{};
};

struct ScenarioResult {
  std::string name{};
  uint64_t messages{};
  bool has_gateway{false};
  GatewayResult gateway{};
  std::vector<RateResult> rates{};
  std::vector<RateResult> scaling{};
};

struct Options {
  std::string mode{"both"};
  std::vector<std::string> scenarios{};
  uint32_t seed{kCanonicalSeed};
  uint64_t count{kCanonicalCount};
  std::string harness_dir{"external/matching-engine-benchmark"};
  std::vector<uint64_t> rates{250'000, 500'000, 1'000'000, 0};
  std::vector<std::size_t> scaling{};
  std::size_t clients{1};
  std::size_t io_threads{1};
  std::string egress{"ring"};
  uint32_t spin_us{0};
  double warmup_fraction{0.05};
  bool subscribe_md{false};
  std::string json_path{};
};

// ---------------------------------------------------------------------------
// gateway mode
// ---------------------------------------------------------------------------

// Counts what came out without touching it further. The gateway's own cost is
// the measurement; a sink that formatted or stored events would be measuring
// the sink.
class TallySink final : public EventSink {
 public:
  void publish(std::span<const Event> events) override {
    m_events += events.size();
    for (const Event& event : events) {
      if (event.type != EventType::Reject) continue;
      if (event.payload.ack.reject_code == RejectCode::UnknownOrder) {
        ++m_unknown_order;
      } else {
        ++m_other;
      }
    }
  }

  uint64_t events() const noexcept { return m_events; }
  uint64_t unknownOrderRejects() const noexcept { return m_unknown_order; }
  uint64_t otherRejects() const noexcept { return m_other; }

 private:
  uint64_t m_events{0};
  uint64_t m_unknown_order{0};
  uint64_t m_other{0};
};

/*
Marshals the whole stream into Commands BEFORE anything is timed.

The flash1 harness does the same thing, and for the same reason: a benchmark
that decodes inside the timed window reports the cost of its own parser mixed
in with the cost of the thing under test, and the two are not separable after
the fact.

recv_ts_ns is the workload's own sequence number rather than a clock reading.
It only feeds market-data timestamps, it satisfies the non-decreasing
requirement by construction, and using it keeps this mode bit-for-bit
reproducible — the property MatchingLoop is built to have.
*/
std::vector<Command> buildCommands(const std::vector<WorkloadRecord>& records,
                                   uint32_t session, uint32_t trader,
                                   uint64_t book) {
  std::vector<Command> commands{};
  commands.reserve(records.size());
  for (const WorkloadRecord& record : records) {
    Command command{};
    command.session_id = session;
    command.trader_id = trader;
    command.book_id = book;
    command.recv_ts_ns = record.seq;
    command.client_order_id = record.order_id;
    command.quantity = record.quantity;
    command.side = record.side == 0 ? Side::Buy : Side::Sell;

    switch (record.type) {
      case MsgKind::kNew:
        command.type = CommandType::NewOrder;
        command.price = encodePrice(record.price_ticks);
        command.tif = record.ioc != 0 ? TimeInForce::Ioc : TimeInForce::Gtc;
        break;
      case MsgKind::kCancel:
        command.type = CommandType::Cancel;
        // 0 means "resolve through client_order_id" — see
        // MatchingLoop::resolveTarget. It is what lets this replay a stream
        // written against the generator's own ids without ever learning the
        // exchange-minted ones.
        command.order_id = 0;
        break;
      case MsgKind::kModify:
        command.type = CommandType::Amend;
        command.order_id = 0;
        command.price = encodePrice(record.price_ticks);
        break;
      default:
        break;
    }
    commands.push_back(command);
  }
  return commands;
}

// Opens a session and creates the book, returning the book id.
uint64_t bootstrap(MatchingLoop& loop, uint32_t session, uint32_t trader) {
  Command open{};
  open.type = CommandType::SessionOpened;
  open.session_id = session;
  open.trader_id = trader;
  loop.handle(open);

  Command create{};
  create.type = CommandType::CreateBook;
  create.session_id = session;
  create.trader_id = trader;
  create.symbol = Exchange::Types::Symbol{std::string_view{kSymbol, 6}};
  loop.handle(create);
  return loop.books().all().front().id.value;
}

/*
Two passes, deliberately.

The throughput pass reads the clock exactly twice, so its msgs/s is comparable
to the flash1 harness's. The latency pass reads it around every command, which
costs ~20-40ns per message — small next to a network round trip but NOT small
next to a gateway dispatch, which is why folding the two together would quietly
inflate the headline throughput by a third.
*/
GatewayResult benchGateway(const std::vector<WorkloadRecord>& records) {
  GatewayResult result{};
  result.messages = records.size();

  constexpr uint32_t kSession{0x01000001};
  constexpr uint32_t kTrader{1};

  // --- throughput ---
  {
    TallySink sink{};
    MatchingLoop loop{sink};
    const uint64_t book{bootstrap(loop, kSession, kTrader)};
    const std::vector<Command> commands{
        buildCommands(records, kSession, kTrader, book)};

    const uint64_t start{monotonicNowNs()};
    loop.handleBatch(commands);
    const uint64_t elapsed{monotonicNowNs() - start};

    result.msgs_per_s = elapsed > 0 ? static_cast<double>(records.size()) *
                                          1e9 / static_cast<double>(elapsed)
                                    : 0.0;
    result.events = sink.events();
    result.unknown_order_rejects = sink.unknownOrderRejects();
    result.other_rejects = sink.otherRejects();
  }

  // --- per-command latency ---
  {
    TallySink sink{};
    MatchingLoop loop{sink};
    const uint64_t book{bootstrap(loop, kSession, kTrader)};
    const std::vector<Command> commands{
        buildCommands(records, kSession, kTrader, book)};

    std::vector<uint64_t> samples{};
    samples.reserve(commands.size());
    for (const Command& command : commands) {
      const uint64_t start{monotonicNowNs()};
      loop.handle(command);
      samples.push_back(monotonicNowNs() - start);
    }
    result.ns = percentilesOf(std::move(samples));
  }

  return result;
}

// ---------------------------------------------------------------------------
// tcp mode
// ---------------------------------------------------------------------------

struct ClientResult {
  std::vector<uint64_t> samples{};
  uint64_t sent{};
  uint64_t acked{};
  uint64_t execs{};
  uint64_t unknown_order_rejects{};
  uint64_t throttled{};
  uint64_t other_rejects{};
  uint64_t ioc_residual_acks{};
  uint64_t desync{};
  uint64_t timed_messages{};
  double elapsed_s{};
  bool disconnected{};
  std::string error{};
};

// One message, already framed. Built before the clock starts, for the same
// reason buildCommands is: a benchmark that encodes inside its timed window is
// partly measuring its own encoder.
struct EncodedMsg {
  uint32_t offset{};
  uint32_t length{};
  uint64_t coid{};
  uint8_t kind{};  // MsgKind::kNew / kCancel / kModify
  bool ioc{false};
};

struct EncodedShare {
  std::vector<std::byte> blob{};
  std::vector<EncodedMsg> msgs{};

  std::size_t size() const noexcept { return msgs.size(); }
};

EncodedShare encodeShare(const std::vector<WorkloadRecord>& records,
                         std::size_t index, std::size_t clients,
                         uint64_t book) {
  EncodedShare share{};
  std::vector<std::byte> frame{};
  uint32_t seq{0};
  for (const WorkloadRecord& record : records) {
    // Shard by order id, so every message about one order — its new, its
    // modify, its cancel — lands on the same connection. Per-order causality
    // survives; cross-order interleaving does not, which is why one client is
    // the default and the fidelity-exact number.
    if (clients > 1 && record.order_id % clients != index) continue;

    frame.clear();
    switch (record.type) {
      case MsgKind::kNew:
        Wire::encodeNewOrder(
            Wire::NewOrderBody{
                .client_order_id = record.order_id,
                .book_id = book,
                .price = encodePrice(record.price_ticks),
                .quantity = record.quantity,
                .side = record.side,
                .tif = static_cast<uint8_t>(record.ioc != 0 ? 1 : 0),
                .flags = 0,
                .pad = {}},
            ++seq, frame);
        break;
      case MsgKind::kCancel:
        Wire::encodeCancel(
            Wire::CancelBody{.client_order_id = record.order_id, .order_id = 0},
            ++seq, frame);
        break;
      case MsgKind::kModify:
        Wire::encodeAmend(
            Wire::AmendBody{.client_order_id = record.order_id,
                            .order_id = 0,
                            .price = encodePrice(record.price_ticks),
                            .quantity = record.quantity},
            ++seq, frame);
        break;
      default:
        continue;
    }
    share.msgs.push_back(
        EncodedMsg{.offset = static_cast<uint32_t>(share.blob.size()),
                   .length = static_cast<uint32_t>(frame.size()),
                   .coid = record.order_id,
                   .kind = record.type,
                   .ioc = record.ioc != 0});
    share.blob.insert(share.blob.end(), frame.begin(), frame.end());
  }
  return share;
}

// One command sent and not yet answered. The deque of these IS the in-flight
// count, which is what keeps it exactly equal to the server's credit count.
struct Outstanding {
  uint64_t scheduled{};
  uint64_t coid{};
  uint8_t kind{};
  bool ioc{false};
};

/*
Drives one connection.

`rate` is this client's share of the offered rate, in messages per second; 0
means unpaced.

--- Why acks are matched in FIFO order rather than by client order id ---

The obvious scheme is a table keyed by client order id. It does not work on
this workload, and the reason is worth recording: the flash1 generator emits
each order's cancel IMMEDIATELY after its new — the median gap between the two
is one message. So at any pipeline depth above 1, an order's new and its cancel
are outstanding together, they carry the same client order id (that is how the
cancel addresses its target), and a table keyed on it cannot tell their acks
apart. Measured on the canonical stream, roughly half of all messages collided.

FIFO is exact instead of approximate. The session is pinned to one matching
thread, commands are handled in ring order, each publishes its own batch, and
egress preserves order — so the ack-family events for a session arrive in
exactly the order their commands were sent. The coid on each ack is then a
consistency check rather than the key, and any mismatch is counted as a desync
instead of being silently absorbed.

The one command that answers with two ack-family events is an IOC order that
leaves a residual: OrderAck, then the CancelAck for the pulled remainder. It is
recognised by its coid matching the IOC new just popped, and skipped.
*/
ClientResult runClient(const Options& options, const EncodedShare& share,
                       uint16_t port, uint64_t rate, uint64_t book,
                       std::size_t warmup) {
  ClientResult result{};
  asio::io_context io{1};
  BenchClient client{io};
  if (!client.connect(port, kApiKey)) {
    result.error = "logon failed";
    return result;
  }

  if (options.subscribe_md) {
    std::vector<std::byte> frame{};
    Wire::encodeSubscribe(
        MsgType::SubscribeMd,
        Wire::SubscribeBody{.book_id = book, .depth = 10, .pad = {}},
        client.nextSeq(), frame);
    client.send(frame);
    // A subscribe is confirmed by the snapshot it triggers, not by an MdAck —
    // MatchingLoop::onSubscribeMd subscribes and snapshots in one handler, and
    // only onUnsubscribeMd emits MdAck. Waiting for the latter here hangs.
    if (!client.await(MsgType::SnapshotEnd).has_value()) {
      result.error = "market-data subscribe failed";
      return result;
    }
  }

  client.setNonBlocking(true);

  std::deque<Outstanding> outstanding{};
  uint64_t timed_from_ts{UINT64_MAX};
  // The coid of the IOC new whose OrderAck was popped most recently, so its
  // trailing residual CancelAck can be told from a real one. 0 is never a
  // client order id here (the generator's are 1-based).
  uint64_t ioc_awaiting_residual{0};
  result.samples.reserve(share.size());

  auto on_ack{[&](MsgType type, uint64_t coid, uint64_t now) {
    if (type == MsgType::CancelAck && coid != 0 &&
        coid == ioc_awaiting_residual) {
      ioc_awaiting_residual = 0;
      ++result.ioc_residual_acks;
      return;
    }
    if (outstanding.empty()) {
      ++result.desync;
      return;
    }
    const Outstanding& front{outstanding.front()};
    const bool expected{
        type == MsgType::Reject ||
        (type == MsgType::OrderAck && front.kind == MsgKind::kNew) ||
        (type == MsgType::CancelAck && front.kind == MsgKind::kCancel) ||
        (type == MsgType::AmendAck && front.kind == MsgKind::kModify)};
    if (!expected || coid != front.coid) {
      ++result.desync;
      return;
    }
    if (type == MsgType::OrderAck && front.ioc) ioc_awaiting_residual = coid;
    if (front.scheduled >= timed_from_ts) {
      result.samples.push_back(now - front.scheduled);
      ++result.timed_messages;
    }
    outstanding.pop_front();
  }};

  auto on_event{[&](MsgType type, const std::optional<Event>& event) {
    if (!event.has_value()) return;
    const uint64_t now{monotonicNowNs()};
    switch (type) {
      case MsgType::OrderAck:
      case MsgType::CancelAck:
      case MsgType::AmendAck:
        ++result.acked;
        on_ack(type, event->payload.ack.client_order_id, now);
        break;
      case MsgType::Reject:
        switch (event->payload.ack.reject_code) {
          case RejectCode::UnknownOrder:
            ++result.unknown_order_rejects;
            break;
          case RejectCode::Throttled:
            ++result.throttled;
            break;
          default:
            ++result.other_rejects;
            break;
        }
        on_ack(type, event->payload.ack.client_order_id, now);
        break;
      case MsgType::ExecReport:
        ++result.execs;
        break;
      default:
        break;
    }
  }};

  const bool paced{rate > 0};
  const uint64_t interval{paced ? 1'000'000'000ull / rate : 0};
  const uint64_t base{monotonicNowNs()};
  uint64_t timed_start{0};
  std::size_t next{0};
  uint64_t last_progress{base};

  while (next < share.size() || !outstanding.empty()) {
    // Read first, always. A driver that writes into a socket whose peer is
    // blocked writing back to it deadlocks, and this is the only ordering
    // that cannot.
    if (client.drain(on_event) > 0) last_progress = monotonicNowNs();
    if (!client.alive()) {
      result.disconnected = true;
      break;
    }

    const uint64_t now{monotonicNowNs()};
    std::size_t bytes_queued{0};
    while (next < share.size() && outstanding.size() < kWindow &&
           bytes_queued < kMaxWriteBytes) {
      /*
      The latency clock starts HERE, at the time this message was due to go
      out — not at the time it actually did. If the driver has fallen behind,
      that backlog belongs in the number. Starting the clock at the actual
      send is coordinated omission: it makes an overloaded system look fast
      by only ever timing the messages it managed to send promptly.
      */
      const uint64_t scheduled{
          paced ? base + static_cast<uint64_t>(next) * interval : now};
      if (paced && scheduled > now) break;

      if (next == warmup) {
        timed_from_ts = scheduled;
        timed_start = now;
      }

      const EncodedMsg& message{share.msgs[next]};
      outstanding.push_back(Outstanding{.scheduled = scheduled,
                                        .coid = message.coid,
                                        .kind = message.kind,
                                        .ioc = message.ioc});
      client.queue(std::span<const std::byte>{
          share.blob.data() + message.offset, message.length});
      bytes_queued += message.length;
      ++next;
      ++result.sent;
    }

    if (!client.pumpWrite()) {
      result.disconnected = true;
      break;
    }
    if (bytes_queued > 0) last_progress = monotonicNowNs();

    if (monotonicNowNs() - last_progress > 30'000'000'000ull) {
      result.error = std::format("stalled with {} in flight after {} sent",
                                 outstanding.size(), result.sent);
      break;
    }
  }

  const uint64_t end{monotonicNowNs()};
  if (timed_start > 0 && end > timed_start) {
    result.elapsed_s = static_cast<double>(end - timed_start) / 1e9;
  }

  boost::system::error_code ec{};
  client.socket().close(ec);
  return result;
}

RateResult benchTcp(const Options& options,
                    const std::vector<WorkloadRecord>& records,
                    uint64_t offered, std::size_t clients) {
  RateResult result{};
  result.offered = offered;
  result.clients = clients;

  ServerConfig config{};
  config.binary_port = 0;
  config.http_port = 0;
  config.io_threads = options.io_threads;
  config.spin_us = options.spin_us;
  config.egress = options.egress;
  config.traders_path = std::string{kTradersPath};

  Server server{config};
  std::string error{};
  if (!server.start(error)) {
    result.error = std::format("server: {}", error);
    return result;
  }

  // The book outlives the session that made it, so one throwaway client can
  // create it and the load clients can all address it by id.
  uint64_t book{0};
  {
    asio::io_context io{1};
    BenchClient setup{io};
    if (!setup.connect(server.binaryPort(), kApiKey)) {
      server.stop();
      result.error = "setup client could not log on";
      return result;
    }
    std::vector<std::byte> frame{};
    Wire::encodeCreateBook(
        Wire::CreateBookBody{
            .symbol = {kSymbol[0], kSymbol[1], kSymbol[2], kSymbol[3],
                       kSymbol[4], kSymbol[5], kSymbol[6], kSymbol[7]},
            .price_scale = 2,
            .pad = {}},
        setup.nextSeq(), frame);
    setup.send(frame);
    const auto created{setup.await(MsgType::CreateBookAck)};
    if (!created.has_value()) {
      server.stop();
      result.error = "create book failed";
      return result;
    }
    book = created->payload.book.book_id;
    boost::system::error_code ec{};
    setup.socket().close(ec);
  }

  std::vector<EncodedShare> shares(clients);
  for (std::size_t i{0}; i < clients; ++i) {
    shares[i] = encodeShare(records, i, clients, book);
  }

  const uint64_t per_client{
      offered == 0 ? 0 : std::max<uint64_t>(1, offered / clients)};
  std::vector<ClientResult> results(clients);
  std::vector<std::thread> threads{};
  threads.reserve(clients);
  for (std::size_t i{0}; i < clients; ++i) {
    const std::size_t warmup{static_cast<std::size_t>(
        static_cast<double>(shares[i].size()) * options.warmup_fraction)};
    threads.emplace_back([&, i, warmup] {
      results[i] = runClient(options, shares[i], server.binaryPort(),
                             per_client, book, warmup);
    });
  }
  for (std::thread& thread : threads) thread.join();

  std::vector<uint64_t> samples{};
  double slowest{0};
  for (const ClientResult& client : results) {
    samples.insert(samples.end(), client.samples.begin(), client.samples.end());
    result.sent += client.sent;
    result.acked += client.acked;
    result.execs += client.execs;
    result.unknown_order_rejects += client.unknown_order_rejects;
    result.throttled += client.throttled;
    result.other_rejects += client.other_rejects;
    result.ioc_residual_acks += client.ioc_residual_acks;
    result.desync += client.desync;
    result.disconnected = result.disconnected || client.disconnected;
    slowest = std::max(slowest, client.elapsed_s);
    if (!client.error.empty() && result.error.empty()) {
      result.error = client.error;
    }
  }
  uint64_t timed{0};
  for (const ClientResult& client : results) timed += client.timed_messages;
  // Wall clock across all clients, not the sum of their individual windows:
  // they run concurrently, so summing would report N times the real rate.
  result.achieved = slowest > 0 ? static_cast<double>(timed) / slowest : 0.0;
  result.ns = percentilesOf(std::move(samples));

  const Server::EgressStats stats{server.egressStats()};
  result.md_drops = stats.market_data_drops;
  result.wakeups = stats.wakeups;
  result.events_drained = stats.events_drained;

  server.stop();
  return result;
}

// ---------------------------------------------------------------------------
// reporting
// ---------------------------------------------------------------------------

std::string rateLabel(uint64_t offered) {
  if (offered == 0) return "saturation";
  if (offered % 1'000'000 == 0)
    return std::format("{}M/s", offered / 1'000'000);
  if (offered % 1000 == 0) return std::format("{}k/s", offered / 1000);
  return std::format("{}/s", offered);
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

std::string jsonPercentiles(const Percentiles& p) {
  return std::format(
      R"({{"p50":{},"p99":{},"p999":{},"max":{},"mean":{:.1f},"count":{}}})",
      p.p50, p.p99, p.p999, p.max, p.mean, p.count);
}

std::string jsonRate(const RateResult& rate) {
  return std::format(
      R"({{"offered":{},"clients":{},"achieved":{:.1f},"ns":{},"sent":{},)"
      R"("acked":{},"execs":{},"unknown_order_rejects":{},"throttled":{},)"
      R"("other_rejects":{},"ioc_residual_acks":{},"desync":{},)"
      R"("md_drops":{},"wakeups":{},"events_drained":{},"disconnected":{},)"
      R"("error":"{}"}})",
      rate.offered, rate.clients, rate.achieved, jsonPercentiles(rate.ns),
      rate.sent, rate.acked, rate.execs, rate.unknown_order_rejects,
      rate.throttled, rate.other_rejects, rate.ioc_residual_acks, rate.desync,
      rate.md_drops, rate.wakeups, rate.events_drained,
      rate.disconnected ? "true" : "false", rate.error);
}

bool writeJson(const Options& options,
               const std::vector<ScenarioResult>& scenarios) {
  std::FILE* out{std::fopen(options.json_path.c_str(), "wb")};
  if (out == nullptr) return false;

  std::string text{};
  text += std::format(
      R"({{"workload":{{"seed":{},"count":{}}},)"
      R"("config":{{"io_threads":{},"egress":"{}","spin_us":{},)"
      R"("window":{},"warmup_fraction":{:.3f},"subscribe_md":{}}},)"
      R"("scenarios":{{)",
      options.seed, options.count, options.io_threads, options.egress,
      options.spin_us, kWindow, options.warmup_fraction,
      options.subscribe_md ? "true" : "false");

  bool first_scenario{true};
  for (const ScenarioResult& scenario : scenarios) {
    if (!first_scenario) text += ",";
    first_scenario = false;
    text += std::format(R"("{}":{{"messages":{})", scenario.name,
                        scenario.messages);
    if (scenario.has_gateway) {
      text += std::format(
          R"(,"gateway":{{"msgs_per_s":{:.1f},"events":{},)"
          R"("unknown_order_rejects":{},"other_rejects":{},"ns":{}}})",
          scenario.gateway.msgs_per_s, scenario.gateway.events,
          scenario.gateway.unknown_order_rejects,
          scenario.gateway.other_rejects, jsonPercentiles(scenario.gateway.ns));
    }
    if (!scenario.rates.empty()) {
      text += R"(,"tcp":{"rates":[)";
      bool first{true};
      for (const RateResult& rate : scenario.rates) {
        if (!first) text += ",";
        first = false;
        text += jsonRate(rate);
      }
      text += "]}";
    }
    if (!scenario.scaling.empty()) {
      text += R"(,"scaling":[)";
      bool first{true};
      for (const RateResult& rate : scenario.scaling) {
        if (!first) text += ",";
        first = false;
        text += jsonRate(rate);
      }
      text += "]";
    }
    text += "}";
  }
  text += "}}\n";

  const bool ok{std::fwrite(text.data(), 1, text.size(), out) == text.size()};
  std::fclose(out);
  return ok;
}

// ---------------------------------------------------------------------------
// arguments
// ---------------------------------------------------------------------------

constexpr std::string_view kUsage{
    R"(usage: net_workload_bench [options]

  --mode gateway|tcp|both   what to measure (default both)
  --scenario NAME           repeatable; default all five flash1 scenarios
  --count N                 messages requested of the generator (default 1000000)
  --seed N                  workload seed (default 23)
  --harness-dir PATH        where the flash1 harness lives
  --rates A,B,C             offered rates for tcp mode; 0 means saturation
                            (default 250000,500000,1000000,0)
  --clients N               connections sharing the stream (default 1)
  --scaling A,B,C           also run saturation at each client count
  --io-threads N            server I/O threads (default 1)
  --egress ring|post        server egress implementation (default ring)
  --spin-us N               matching-thread idle spin (default 0)
  --warmup-fraction F       leading share of the stream left untimed (0.05)
  --subscribe-md            also subscribe each client to L2 depth
  --json PATH               write machine-readable results
  --help)"};

bool parseList(std::string_view text, std::vector<uint64_t>& out) {
  out.clear();
  while (!text.empty()) {
    const std::size_t comma{text.find(',')};
    const std::string_view item{text.substr(0, comma)};
    uint64_t value{0};
    const auto end{item.data() + item.size()};
    const auto parsed{std::from_chars(item.data(), end, value)};
    if (parsed.ec != std::errc{} || parsed.ptr != end) return false;
    out.push_back(value);
    if (comma == std::string_view::npos) break;
    text.remove_prefix(comma + 1);
  }
  return !out.empty();
}

bool parseArgs(int argc, char** argv, Options& options) {
  for (int i{1}; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    auto next{[&]() -> std::string_view {
      return i + 1 < argc ? std::string_view{argv[++i]} : std::string_view{};
    }};
    if (arg == "--mode") {
      options.mode = std::string{next()};
    } else if (arg == "--scenario") {
      options.scenarios.emplace_back(next());
    } else if (arg == "--count") {
      options.count = std::stoull(std::string{next()});
    } else if (arg == "--seed") {
      options.seed = static_cast<uint32_t>(std::stoul(std::string{next()}));
    } else if (arg == "--harness-dir") {
      options.harness_dir = std::string{next()};
    } else if (arg == "--rates") {
      if (!parseList(next(), options.rates)) return false;
    } else if (arg == "--clients") {
      options.clients = std::stoull(std::string{next()});
    } else if (arg == "--scaling") {
      std::vector<uint64_t> values{};
      if (!parseList(next(), values)) return false;
      options.scaling.clear();
      for (const uint64_t value : values) {
        options.scaling.push_back(static_cast<std::size_t>(value));
      }
    } else if (arg == "--io-threads") {
      options.io_threads = std::stoull(std::string{next()});
    } else if (arg == "--egress") {
      options.egress = std::string{next()};
    } else if (arg == "--spin-us") {
      options.spin_us = static_cast<uint32_t>(std::stoul(std::string{next()}));
    } else if (arg == "--warmup-fraction") {
      options.warmup_fraction = std::stod(std::string{next()});
    } else if (arg == "--subscribe-md") {
      options.subscribe_md = true;
    } else if (arg == "--json") {
      options.json_path = std::string{next()};
    } else if (arg == "--help" || arg == "-h") {
      std::println("{}", kUsage);
      std::exit(0);
    } else {
      std::println(stderr, "error: unknown flag '{}'", arg);
      std::println(stderr, "{}", kUsage);
      return false;
    }
  }
  if (options.scenarios.empty()) {
    options.scenarios = {"static", "normal", "swing-25", "swing-40",
                         "flash-crash"};
  }
  if (options.clients == 0) options.clients = 1;
  return options.mode == "gateway" || options.mode == "tcp" ||
         options.mode == "both";
}
}  // namespace

int main(int argc, char** argv) {
  Options options{};
  if (!parseArgs(argc, argv, options)) return 2;

  const bool do_gateway{options.mode != "tcp"};
  const bool do_tcp{options.mode != "gateway"};

  std::vector<ScenarioResult> results{};
  int failures{0};

  for (const std::string& scenario : options.scenarios) {
    const auto workload{ensureWorkload(options.harness_dir, scenario,
                                       options.seed, options.count)};
    if (!workload.has_value()) {
      std::println(stderr, "error: {}", workload.error());
      return 1;
    }
    const std::vector<WorkloadRecord>& records{*workload};

    ScenarioResult result{};
    result.name = scenario;
    result.messages = records.size();
    std::println("");
    std::println("=== {} — {} messages (seed {}) ===", scenario, records.size(),
                 options.seed);

    if (do_gateway) {
      result.gateway = benchGateway(records);
      result.has_gateway = true;
      const GatewayResult& g{result.gateway};
      std::println(
          "{:<22} {:>8.3f} M msgs/s   {} events   {} unknown-order rejects",
          "gateway", g.msgs_per_s / 1e6, g.events, g.unknown_order_rejects);
      std::println(
          "{:<22} p50={:>8.0f}ns p99={:>8.0f}ns p99.9={:>9.0f}ns "
          "max={:>9.0f}ns mean={:>8.0f}ns",
          "  per command", static_cast<double>(g.ns.p50),
          static_cast<double>(g.ns.p99), static_cast<double>(g.ns.p999),
          static_cast<double>(g.ns.max), g.ns.mean);

      // The generator plants ~2% duplicate cancels and stale modifies. None at
      // all means the stream did not really replay; a flood means the run fell
      // into a fast failure path and its throughput is meaningless.
      if (g.unknown_order_rejects == 0) {
        std::println(stderr,
                     "FAIL: no UnknownOrder rejects — the duplicate-cancel "
                     "path was never exercised, so this did not replay the "
                     "flash1 stream");
        ++failures;
      } else if (g.unknown_order_rejects > records.size() / 10) {
        std::println(stderr,
                     "FAIL: {} of {} messages rejected as UnknownOrder — this "
                     "is a failure path, not a result",
                     g.unknown_order_rejects, records.size());
        ++failures;
      }
    }

    if (do_tcp) {
      for (const uint64_t offered : options.rates) {
        const RateResult rate{
            benchTcp(options, records, offered, options.clients)};
        result.rates.push_back(rate);
        std::println(
            "{:<22} {:>8.3f} M msgs/s   p50={:>7.1f}us p99={:>8.1f}us "
            "p99.9={:>9.1f}us max={:>9.1f}us",
            std::format("tcp {} c={}", rateLabel(offered), options.clients),
            rate.achieved / 1e6, static_cast<double>(rate.ns.p50) / 1000.0,
            static_cast<double>(rate.ns.p99) / 1000.0,
            static_cast<double>(rate.ns.p999) / 1000.0,
            static_cast<double>(rate.ns.max) / 1000.0);
        if (rate.throttled > 0 || rate.md_drops > 0) {
          std::println("{:<22} throttled={} md-drops={}", "", rate.throttled,
                       rate.md_drops);
        }
        if (rate.disconnected || !rate.error.empty()) {
          std::println(stderr, "FAIL: {} at {}: {}", scenario,
                       rateLabel(offered),
                       rate.error.empty() ? "disconnected" : rate.error);
          ++failures;
        }
        // FIFO ack matching is exact, not a heuristic: a desync means the
        // stream this measured is not the stream it thinks it sent, so the
        // latency numbers describe nothing in particular.
        if (rate.desync > 0) {
          std::println(stderr, "FAIL: {} at {}: {} desynced ack(s)", scenario,
                       rateLabel(offered), rate.desync);
          ++failures;
        }
      }

      for (const std::size_t clients : options.scaling) {
        const RateResult rate{benchTcp(options, records, 0, clients)};
        result.scaling.push_back(rate);
        std::println("{:<22} {:>8.3f} M msgs/s   p50={:>7.1f}us p99={:>8.1f}us",
                     std::format("scaling c={}", clients), rate.achieved / 1e6,
                     static_cast<double>(rate.ns.p50) / 1000.0,
                     static_cast<double>(rate.ns.p99) / 1000.0);
        if (rate.disconnected || !rate.error.empty() || rate.desync > 0) {
          std::println(stderr, "FAIL: {} scaling c={}: {}", scenario, clients,
                       rate.error.empty() ? (rate.desync > 0 ? "desynced acks"
                                                             : "disconnected")
                                          : rate.error);
          ++failures;
        }
      }
    }

    results.push_back(std::move(result));
  }

  if (!options.json_path.empty()) {
    if (!writeJson(options, results)) {
      std::println(stderr, "error: could not write {}", options.json_path);
      return 1;
    }
    std::println("");
    std::println("json: {}", options.json_path);
  }

  if (failures > 0) {
    std::println(stderr, "");
    std::println(stderr, "{} check(s) failed", failures);
    return 1;
  }
  return 0;
}
