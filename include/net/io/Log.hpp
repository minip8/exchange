#pragma once

#include <cstdio>
#include <format>
#include <mutex>
#include <string>
#include <utility>

namespace Exchange::Net {
/*
Serialized logging for the multi-threaded server.

`std::println` is NOT safe to call concurrently on this toolchain.
libstdc++'s _File_sink reaches into the FILE's buffer pointers directly
rather than going through the locked stdio path, so two I/O threads logging
at the same time race on _IO_write_ptr. ThreadSanitizer flags it, and the
consequence is not theoretical — corrupted buffer pointers produce garbled or
lost output.

So: format outside the lock (the expensive part, and it touches nothing
shared), then take the lock only for the write. Logging here is per
connection and per lifecycle event, never per message, so the mutex is never
contended in practice — and if it ever were, that would mean the server was
logging on a hot path, which is its own bug.
*/
// Named logLine, not log: `log` collides with the one from <cmath> anywhere
// this is called from outside namespace Exchange::Net.
template <typename... Args>
void logLine(std::format_string<Args...> fmt, Args&&... args) {
  static std::mutex mutex{};
  const std::string line{std::format(fmt, std::forward<Args>(args)...)};
  const std::lock_guard guard{mutex};
  std::fwrite(line.data(), 1, line.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

template <typename... Args>
void logError(std::format_string<Args...> fmt, Args&&... args) {
  static std::mutex mutex{};
  const std::string line{std::format(fmt, std::forward<Args>(args)...)};
  const std::lock_guard guard{mutex};
  std::fwrite(line.data(), 1, line.size(), stderr);
  std::fputc('\n', stderr);
  std::fflush(stderr);
}
}  // namespace Exchange::Net
