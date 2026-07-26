#pragma once

#include <algorithm>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <print>
#include <span>
#include <string_view>
#include <vector>

#include "net/core/Event.hpp"
#include "net/wire/BinaryProtocol.hpp"
#include "net/wire/MessageNames.hpp"

namespace Exchange::Bench {
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using Exchange::Net::Event;
using Exchange::Net::MsgType;
namespace Wire = Exchange::Net::Binary;

/*
The pieces every network benchmark in this repo needs: a monotonic clock, a
percentile summary, and a minimal synchronous loopback client that gets binary
framing right.

Percentiles rather than Google Benchmark's mean/stddev because network latency
is a distribution whose tail IS the measurement — nobody cares that the median
order acks in 30us if one in a thousand takes 5ms. Sorting a vector<uint64_t>
needs no dependency and is impossible to get subtly wrong.
*/

/*
The benchmark's own clock, and deliberately NOT Net::nowNs().

steady_clock, because everything here measures an ELAPSED interval and a
monotonic clock cannot be stepped backwards by NTP mid-run. Net::nowNs() is a
system_clock, because it stamps Command::recv_ts_ns — a wall-clock timestamp
that reaches clients on the wire and is meant to be a real date.

Two different jobs, two different clocks. The explicit name is here because
the two were previously both called nowNs(), in different namespaces, and
nothing said which one a call site meant to get.
*/
inline uint64_t monotonicNowNs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

struct Percentiles {
  uint64_t p50{};
  uint64_t p99{};
  uint64_t p999{};
  uint64_t max{};
  double mean{};
  std::size_t count{};
};

// Takes the sample by value: sorting is destructive and the caller almost
// always wants its vector back untouched for a second measurement.
inline Percentiles percentilesOf(std::vector<uint64_t> samples) {
  if (samples.empty()) return {};
  std::ranges::sort(samples);
  auto at{[&](double quantile) {
    const auto index{static_cast<std::size_t>(
        quantile * static_cast<double>(samples.size() - 1))};
    return samples[index];
  }};
  double total{0};
  for (const uint64_t sample : samples) total += static_cast<double>(sample);
  return Percentiles{.p50 = at(0.50),
                     .p99 = at(0.99),
                     .p999 = at(0.999),
                     .max = samples.back(),
                     .mean = total / static_cast<double>(samples.size()),
                     .count = samples.size()};
}

// Returns what it printed, so a caller that also wants to record the numbers
// does not have to sort twice.
inline Percentiles report(std::string_view label,
                          const std::vector<uint64_t>& samples) {
  const Percentiles p{percentilesOf(samples)};
  std::println(
      "{:<22} n={:<8} p50={:>8.2f}us p99={:>8.2f}us "
      "p99.9={:>9.2f}us max={:>9.2f}us mean={:>8.2f}us",
      label, samples.size(), static_cast<double>(p.p50) / 1000.0,
      static_cast<double>(p.p99) / 1000.0, static_cast<double>(p.p999) / 1000.0,
      static_cast<double>(p.max) / 1000.0, p.mean / 1000.0);
  return p;
}

/*
A minimal binary-protocol client for loopback benchmarking.

Two operating modes, because the two phases of a benchmark want opposite
things. Setup (logon, create book) wants `send` + `await`: blocking, one
message at a time, trivially correct. The load phase wants `queue` +
`pumpWrite` + `drain`: fully non-blocking, so the driver can always read
before it writes and never deadlock against a server that is trying to write
to it at the same time.

Call setNonBlocking(true) once between the two. Mixing them is a bug — `await`
on a non-blocking socket spins.
*/
class BenchClient {
 public:
  explicit BenchClient(asio::io_context& io) : m_socket(io) {}

  bool connect(uint16_t port, std::string_view key) {
    boost::system::error_code ec{};
    m_socket.connect(tcp::endpoint{asio::ip::make_address("127.0.0.1"), port},
                     ec);
    if (ec) return false;
    m_socket.set_option(tcp::no_delay{true}, ec);

    std::vector<std::byte> frame{};
    Wire::encodeLogon(key, false, ++m_seq, frame);
    asio::write(m_socket, asio::buffer(frame), ec);
    if (ec) return false;
    m_alive = true;
    return await(MsgType::LogonAck).has_value();
  }

  uint32_t nextSeq() { return ++m_seq; }
  bool alive() const noexcept { return m_alive; }
  tcp::socket& socket() { return m_socket; }

  void setNonBlocking(bool enable) {
    boost::system::error_code ec{};
    m_socket.non_blocking(enable, ec);
  }

  // --- blocking, setup phase ---

  void send(const std::vector<std::byte>& frame) {
    boost::system::error_code ec{};
    asio::write(m_socket, asio::buffer(frame), ec);
    if (ec) m_alive = false;
  }

  std::optional<Event> await(MsgType wanted) {
    for (;;) {
      MsgType type{};
      std::optional<Event> event{};
      while (nextFrame(type, event)) {
        if (type == wanted) return event;
      }
      boost::system::error_code ec{};
      const std::size_t bytes{m_socket.read_some(
          asio::buffer(m_buffer.data() + m_size, m_buffer.size() - m_size),
          ec)};
      if (ec) {
        m_alive = false;
        return std::nullopt;
      }
      m_size += bytes;
    }
  }

  // --- non-blocking, load phase ---

  void queue(std::span<const std::byte> frame) {
    m_pending.insert(m_pending.end(), frame.begin(), frame.end());
  }

  std::size_t queued() const noexcept { return m_pending.size() - m_sent; }

  // Writes as much of the pending buffer as the socket will take right now.
  // Returns false only on a hard error; a full send buffer is not an error,
  // it is the whole reason this is not asio::write.
  bool pumpWrite() {
    while (m_sent < m_pending.size()) {
      boost::system::error_code ec{};
      const std::size_t written{m_socket.write_some(
          asio::buffer(m_pending.data() + m_sent, m_pending.size() - m_sent),
          ec)};
      if (ec == asio::error::would_block || ec == asio::error::try_again) {
        return true;
      }
      if (ec) {
        m_alive = false;
        return false;
      }
      m_sent += written;
    }
    m_pending.clear();
    m_sent = 0;
    return true;
  }

  /*
  Consumes every frame currently readable and hands each to `on_event`, which
  is called as on_event(MsgType, const std::optional<Event>&). Returns the
  number of frames delivered; 0 means the socket had nothing right now, which
  is the normal case in a paced loop and is not an error.
  */
  template <typename Fn>
  std::size_t drain(Fn&& on_event) {
    std::size_t delivered{0};
    for (;;) {
      MsgType type{};
      std::optional<Event> event{};
      while (nextFrame(type, event)) {
        on_event(type, event);
        ++delivered;
      }
      if (m_size == m_buffer.size()) m_buffer.resize(m_buffer.size() * 2);
      boost::system::error_code ec{};
      const std::size_t bytes{m_socket.read_some(
          asio::buffer(m_buffer.data() + m_size, m_buffer.size() - m_size),
          ec)};
      if (ec == asio::error::would_block || ec == asio::error::try_again) {
        return delivered;
      }
      if (ec) {
        m_alive = false;
        return delivered;
      }
      m_size += bytes;
    }
  }

 private:
  // Pops one complete frame off the front of the receive buffer. False means
  // "not a whole frame yet"; the caller then reads more, or gives up.
  bool nextFrame(MsgType& type, std::optional<Event>& event) {
    if (m_size < Wire::kHeaderSize) return false;
    Wire::Header header{};
    std::memcpy(&header, m_buffer.data(), Wire::kHeaderSize);
    if (header.length < Wire::kHeaderSize || m_size < header.length) {
      return false;
    }
    event = Wire::decodeEvent(
        header.type,
        std::span<const std::byte>{m_buffer.data() + Wire::kHeaderSize,
                                   header.length - Wire::kHeaderSize});
    type = header.type;
    std::memmove(m_buffer.data(), m_buffer.data() + header.length,
                 m_size - header.length);
    m_size -= header.length;
    return true;
  }

  tcp::socket m_socket;
  uint32_t m_seq{0};
  bool m_alive{false};
  std::vector<std::byte> m_buffer = std::vector<std::byte>(256 * 1024);
  std::size_t m_size{0};
  // Outbound bytes not yet accepted by the kernel, and how far into them the
  // socket has got. Not erased from the front on every partial write: that
  // would be quadratic on exactly the path that is already under pressure.
  std::vector<std::byte> m_pending{};
  std::size_t m_sent{0};
};
}  // namespace Exchange::Bench
