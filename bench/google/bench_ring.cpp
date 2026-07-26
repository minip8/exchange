/*
Ring microbenchmarks.

Three things are worth measuring here, and they map directly onto the three
design decisions in SpscRing:

  - single-threaded push/pop, which isolates the pure bookkeeping cost;
  - the batch-size sweep, which shows what amortizing one acquire load and
    one release store over N items actually buys;
  - the two-thread ping-pong, which is where the cached read/write indices
    pay off. Without them every operation drags the other core's cache line
    across the interconnect, and the RTT roughly doubles.

Build and run Release only — like every benchmark in this repo, a sanitized
number here is meaningless:

    cmake --build --preset bench
    ./build/release/bench/google/exchange_bench --benchmark_filter='BM_Ring.*'
*/
#include <benchmark/benchmark.h>

#include <array>
#include <atomic>
#include <memory>
#include <thread>

#include "net/core/CacheLine.hpp"
#include "net/core/Command.hpp"
#include "net/core/Event.hpp"
#include "net/core/SpscRing.hpp"
#include "net/core/Tuning.hpp"

using Exchange::Net::Command;
using Exchange::Net::cpuPause;
using Exchange::Net::Event;
using Exchange::Net::kIngressRingCapacity;
using Exchange::Net::SpscRing;

namespace {

// Push and immediately pop, so the ring never fills and the measurement is
// the bookkeeping alone: two relaxed loads, one store each way, and a
// 64-byte copy.
void BM_RingPushPop(benchmark::State& state) {
  auto ring{std::make_unique<SpscRing<Command, kIngressRingCapacity>>()};
  Command in{};
  Command out{};
  uint64_t i{0};
  for (auto _ : state) {
    in.recv_ts_ns = ++i;
    benchmark::DoNotOptimize(ring->tryPush(in));
    benchmark::DoNotOptimize(ring->tryPop(out));
  }
  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(state.iterations() *
                          static_cast<int64_t>(sizeof(Command)));
}
BENCHMARK(BM_RingPushPop);

// The same work done in batches. The interesting axis is not raw throughput
// but how quickly the per-item cost flattens — the atomics are amortized
// after only a handful of items, and past that it is pure memcpy.
void BM_RingBatch(benchmark::State& state) {
  const std::size_t batch{static_cast<std::size_t>(state.range(0))};
  auto ring{std::make_unique<SpscRing<Command, kIngressRingCapacity>>()};

  std::vector<Command> in(batch);
  std::vector<Command> out(batch);
  for (std::size_t i{0}; i < batch; ++i) in[i].recv_ts_ns = i;

  for (auto _ : state) {
    benchmark::DoNotOptimize(ring->tryPushBatch(in));
    benchmark::DoNotOptimize(ring->tryPopBatch(out));
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(batch));
  state.SetBytesProcessed(state.iterations() *
                          static_cast<int64_t>(batch * sizeof(Command)));
}
BENCHMARK(BM_RingBatch)->RangeMultiplier(2)->Range(1, 256);

/*
Two-thread round trip: a command crosses one ring and an event comes back on
another, which is exactly the shape of the real ingress/egress pair.

This is the number the cached-index optimization exists for. Both threads
spin rather than sleep, so what is being measured is the cache-coherence
round trip and nothing else.
*/
void BM_RingPingPong(benchmark::State& state) {
  auto to_server{std::make_unique<SpscRing<Command, 1024>>()};
  auto to_client{std::make_unique<SpscRing<Event, 1024>>()};
  std::atomic<bool> stop{false};

  std::thread responder{[&] {
    Command command{};
    while (!stop.load(std::memory_order_relaxed)) {
      if (!to_server->tryPop(command)) {
        cpuPause();
        continue;
      }
      Event event{};
      event.payload.raw[0] = command.recv_ts_ns;
      while (!to_client->tryPush(event)) {
        if (stop.load(std::memory_order_relaxed)) return;
        cpuPause();
      }
    }
  }};

  Command command{};
  Event event{};
  uint64_t i{0};
  for (auto _ : state) {
    command.recv_ts_ns = ++i;
    while (!to_server->tryPush(command)) cpuPause();
    while (!to_client->tryPop(event)) cpuPause();
    benchmark::DoNotOptimize(event);
  }

  stop.store(true, std::memory_order_relaxed);
  // Unblock the responder if it is parked on an empty ingress ring.
  static_cast<void>(to_server->tryPush(command));
  responder.join();

  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RingPingPong)->UseRealTime();

// Sanity check that the 64-byte messages are what is actually being moved.
static_assert(sizeof(Command) == Exchange::Net::kCacheLine);
static_assert(sizeof(Event) == Exchange::Net::kCacheLine);

}  // namespace
