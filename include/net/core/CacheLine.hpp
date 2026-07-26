#pragma once

#include <cstddef>
#include <new>

namespace Exchange::Net {
/*
The false-sharing granule.

GCC warns (-Winterference-size) that this constant's value is not stable
across compiler versions, and so must not appear in a type's layout that
crosses a separately-compiled boundary. Everything that uses kCacheLine here
lives in one binary built from one toolchain, so the hazard does not apply —
and the warning is silenced in this one scope rather than switched off for
the project, where it would be worth hearing.
*/
#ifdef __cpp_lib_hardware_interference_size
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winterference-size"
inline constexpr std::size_t kCacheLine{
    std::hardware_destructive_interference_size};
#pragma GCC diagnostic pop
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
