#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Exchange::Bench {
/*
Loads the flash1 harness's order stream so the networking benchmarks can
replay it over the wire.

The stream is NOT internal to the harness binary. `./generator` writes it to
`orders_<scenario>_s<seed>_n<count>.bin` and the harness only shells out when
that file is missing (see harness.cpp's workload_path block). So the same
bytes the flash1 conformance run sees can be fed through the gateway and the
TCP path, which is the entire point: one workload, five volatility regimes,
and network numbers that are directly comparable to engine numbers.

Deliberately does NOT include external/.../src/harness.h. The record layout is
a stable on-disk format, and redeclaring it means this target compiles whether
or not the harness is checked out — only *running* needs external/.
*/

// The 40-byte on-disk record. Field order and sizes are the file format, not
// a choice; the static_assert is what catches a silent layout change.
struct WorkloadRecord {
  uint8_t type;  // 0 = NEW, 1 = CANCEL, 2 = MODIFY
  uint8_t side;  // 0 = buy, 1 = sell — same encoding as net/core/Side.hpp
  uint8_t ioc;
  uint8_t pad;
  uint32_t quantity;
  uint64_t seq;       // dense, 0-based, assigned at generation time
  uint64_t order_id;  // dense, 1-based; CANCEL/MODIFY name their target
  int64_t price_ticks;
  int64_t reserved;
};
static_assert(sizeof(WorkloadRecord) == 40,
              "the on-disk record is 40 bytes; a hole here would misparse "
              "every message after the first");

namespace MsgKind {
inline constexpr uint8_t kNew{0};
inline constexpr uint8_t kCancel{1};
inline constexpr uint8_t kModify{2};
}  // namespace MsgKind

inline constexpr uint64_t kWorkloadMagic{0x4D4542575F303031ull};  // "MEBW_001"
inline constexpr uint32_t kWorkloadVersion{1};

// The harness's own defaults. Changing either produces a different file name
// and therefore a different (freshly generated) stream.
inline constexpr uint32_t kCanonicalSeed{23};
inline constexpr uint64_t kCanonicalCount{1'000'000};

/*
Maps a signed tick to the engine's unsigned OrderPrice domain.

This is the load-bearing fidelity guarantee of the whole exercise: it is
byte-identical to `encodePrice` in bench/flash1/adapter.cpp, so a tick lands on
the same OrderPrice whether it arrives through the harness ABI or through the
binary protocol. Flipping the sign bit maps int64 order onto uint64 order,
which is what keeps the book's price comparisons meaning the same thing.

Copied rather than shared because the flash1 adapter is deliberately
dependency-free — it compiles OrderBook.cpp directly and links nothing. If one
of the two ever changes, the scenarios stop being comparable, which is the
kind of thing worth a comment in both places.
*/
constexpr uint64_t encodePrice(int64_t ticks) noexcept {
  return static_cast<uint64_t>(ticks) ^ (1ull << 63);
}

// orders_<scenario>_s<seed>_n<count>.bin, resolved against `harness_dir`.
// `count` is the count REQUESTED of the generator, not the number of records
// the file ends up holding — lifecycle expansion roughly doubles it.
std::filesystem::path workloadPath(const std::filesystem::path& harness_dir,
                                   std::string_view scenario, uint32_t seed,
                                   uint64_t count);

// Reads and validates one workload file. Mirrors the harness's own
// load_workload, including rejecting a record with type > 2.
std::expected<std::vector<WorkloadRecord>, std::string> loadWorkload(
    const std::filesystem::path& path);

// loadWorkload, but generates the file first if it is missing — the same
// shell-out to ./generator the harness does, so the canonical files are shared
// rather than duplicated.
std::expected<std::vector<WorkloadRecord>, std::string> ensureWorkload(
    const std::filesystem::path& harness_dir, std::string_view scenario,
    uint32_t seed, uint64_t count);
}  // namespace Exchange::Bench
