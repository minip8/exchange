#pragma once

#include <cstddef>
#include <new>

namespace Exchange::Net {
/*
The false-sharing granule. `std::hardware_destructive_interference_size` is
the standard spelling and is available here, but it is only a hint and some
toolchains warn about ABI-sensitivity when it is used across a boundary — we
only use it within this binary, so that is not a concern.
*/
#ifdef __cpp_lib_hardware_interference_size
inline constexpr std::size_t kCacheLine{
    std::hardware_destructive_interference_size};
#else
inline constexpr std::size_t kCacheLine{64};
#endif

static_assert(kCacheLine >= 32, "implausible cache line size");

/*
A hint to the CPU that this is a spin-wait iteration: on x86 it drops the
pipeline-flush penalty on loop exit and yields the SMT sibling; on ARM it is
the `yield` hint. Not a fence, and not a scheduling yield.
*/
#if defined(__x86_64__) || defined(__i386__)
inline void cpuPause() noexcept { __builtin_ia32_pause(); }
#elif defined(__aarch64__)
inline void cpuPause() noexcept { __asm__ __volatile__("yield" ::: "memory"); }
#else
inline void cpuPause() noexcept {}
#endif
}  // namespace Exchange::Net
