#include "Flash1Workload.hpp"

#include <cstdio>
#include <cstdlib>
#include <format>

namespace Exchange::Bench {
namespace fs = std::filesystem;

namespace {
// The harness caps this; a larger declared count means the file is corrupt
// rather than merely big, and we would otherwise try to allocate on its word.
constexpr uint32_t kMaxRecords{200'000'000};

// The generator path goes through /bin/sh, so a quote in it would be a command
// injection rather than a bad file name. Refusing beats escaping: no legitimate
// harness directory contains one.
bool shellSafe(const std::string& text) {
  return text.find_first_of("'\"`$\\\n") == std::string::npos;
}
}  // namespace

fs::path workloadPath(const fs::path& harness_dir, std::string_view scenario,
                      uint32_t seed, uint64_t count) {
  return harness_dir /
         std::format("orders_{}_s{}_n{}.bin", scenario, seed, count);
}

std::expected<std::vector<WorkloadRecord>, std::string> loadWorkload(
    const fs::path& path) {
  std::FILE* file{std::fopen(path.c_str(), "rb")};
  if (file == nullptr) {
    return std::unexpected(std::format("cannot open {}", path.string()));
  }

  uint64_t magic{0};
  uint32_t version{0};
  uint32_t count{0};
  const bool header_ok{std::fread(&magic, 8, 1, file) == 1 &&
                       std::fread(&version, 4, 1, file) == 1 &&
                       std::fread(&count, 4, 1, file) == 1};
  if (!header_ok || magic != kWorkloadMagic) {
    std::fclose(file);
    return std::unexpected(
        std::format("{} is not a workload file (bad magic)", path.string()));
  }
  if (version != kWorkloadVersion) {
    std::fclose(file);
    return std::unexpected(std::format(
        "{} is workload version {}; this bench expects {}. Re-generate it "
        "with the matching ./generator.",
        path.string(), version, kWorkloadVersion));
  }
  if (count == 0 || count > kMaxRecords) {
    std::fclose(file);
    return std::unexpected(
        std::format("{} declares {} records; valid range is [1, {}]",
                    path.string(), count, kMaxRecords));
  }

  std::vector<WorkloadRecord> records(count);
  const bool read_ok{
      std::fread(records.data(), sizeof(WorkloadRecord), count, file) == count};
  std::fclose(file);
  if (!read_ok) {
    return std::unexpected(std::format("short read from {}", path.string()));
  }

  for (uint32_t i{0}; i < count; ++i) {
    if (records[i].type > MsgKind::kModify) {
      return std::unexpected(std::format(
          "{} record {} has invalid type {} (expected 0=NEW, 1=CANCEL, "
          "2=MODIFY); the file is corrupt",
          path.string(), i, records[i].type));
    }
  }
  return records;
}

std::expected<std::vector<WorkloadRecord>, std::string> ensureWorkload(
    const fs::path& harness_dir, std::string_view scenario, uint32_t seed,
    uint64_t count) {
  const fs::path path{workloadPath(harness_dir, scenario, seed, count)};
  if (fs::exists(path)) return loadWorkload(path);

  const fs::path generator{harness_dir / "generator"};
  if (!fs::exists(generator)) {
    return std::unexpected(std::format(
        "{} is missing and {} does not exist — run scripts/fetch_harness.sh "
        "to clone and build the flash1 harness first",
        path.string(), generator.string()));
  }
  if (!shellSafe(generator.string()) || !shellSafe(path.string())) {
    return std::unexpected(
        "the harness directory contains shell metacharacters; move it "
        "somewhere with a plain path");
  }

  // Absolute paths on both sides, so unlike the harness (which runs with its
  // own directory as the CWD) this does not care where it was invoked from.
  const std::string command{std::format("'{}' {} '{}' {} {}",
                                        generator.string(), scenario,
                                        path.string(), count, seed)};
  std::fprintf(stderr, "generating workload: %s\n", command.c_str());
  if (std::system(command.c_str()) != 0) {
    return std::unexpected(
        std::format("generator failed for scenario '{}'", scenario));
  }
  return loadWorkload(path);
}
}  // namespace Exchange::Bench
