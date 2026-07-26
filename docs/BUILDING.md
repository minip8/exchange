# Building

Everything is driven through `CMakePresets.json`. There are three build trees,
one per configuration, and each is addressed **by its preset name** — never by
an ad-hoc `-B` path.

- [Prerequisites](#prerequisites)
- [Quick start](#quick-start)
- [The three trees](#the-three-trees)
- [Build presets](#build-presets)
- [Where binaries land](#where-binaries-land)
- [Debug vs Release: which to use when](#debug-vs-release-which-to-use-when)
- [Options](#options)
- [Formatting](#formatting)
- [Troubleshooting](#troubleshooting)

## Prerequisites

| Tool | Needed for | Notes |
|---|---|---|
| **g++ 14+** | everything | C++26: `std::expected`, `std::print`, `std::function_ref`, deducing `this`. Developed against GCC 16 trunk. |
| **CMake 4.3.3+** | everything | `cmake_minimum_required(VERSION 4.3.3)`. |
| **Ninja** | everything | All presets pin the Ninja generator. |
| **Boost headers 1.83+** | the networking layer | `apt install libboost-dev`. Header-only (Asio + Beast + JSON) — no Boost libs are linked. |
| **`uv`** | benchmark plots, `scripts/ws_e2e.py` | Python deps are declared inline (PEP 723); no venv to manage. |
| **python3 3.8+** | the flash1 harness | Only for `scripts/fetch_harness.sh`. |

Boost must resolve in **CONFIG mode** — CMake 4.4 removed `FindBoost.cmake`, so
module mode hard-fails. If Boost is missing you can still build the engine and
benchmarks: configure with `-DEXCHANGE_BUILD_NET=OFF`.

## Quick start

```bash
# Configure once per tree (re-running is idempotent and cheap)
cmake --preset debug
cmake --preset release

# Build everything
cmake --build --preset debug
cmake --build --preset release
```

`cmake --preset` resolves `CMakePresets.json` from the **current working
directory**, so run these from the repo root. (Scripts that may be invoked from
anywhere `cd` to the root themselves.)

## The three trees

Each tree is **single-config**: artifacts land directly in the tree with no
per-config subdirectory, and `--config` is never used. Passing `--config` to a
single-config tree is silently ignored, which is exactly the failure this layout
exists to prevent.

| Preset | Tree | Flags | Use it for |
|---|---|---|---|
| `debug` | `build/debug/` | `-Og -g3`, ASan + UBSan + LSan, `_GLIBCXX_DEBUG`, `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` | Day-to-day development. The default. |
| `tsan` | `build/tsan/` | `-Og -g3`, ThreadSanitizer, `_GLIBCXX_DEBUG` | The **only** config that can validate the SPSC rings' memory ordering. TSan cannot coexist with ASan, hence a separate tree. |
| `release` | `build/release/` | `-O3`, no sanitizers | The **only** config perf numbers may come from. Also what the flash1 adapter and the servers ship as. |

Which sanitizers the Debug config carries is chosen by the `EXCHANGE_SANITIZER`
cache variable (`address` | `thread` | `none`); the `debug` and `tsan` presets
just pin it to different values.

```bash
cmake --preset tsan
cmake --build --preset tsan
```

## Build presets

`cmake --build --preset <name>` — the first three build everything in their
tree, the rest build a single target.

| Preset | Config | Builds |
|---|---|---|
| `debug` | Debug+ASan | everything |
| `tsan` | Debug+TSan | everything |
| `release` | Release | everything |
| `bench` | Release | `exchange_bench` (Google Benchmark suite) |
| `flash1` | Release | `flash1_adapter` (the conformance harness adapter) |
| `server` | Release | `exchange_server` |
| `cli` | Release | `exchange_cli` |
| `smoke` | Debug+ASan | `net_smoke` |
| `smoke-tsan` | Debug+TSan | `net_smoke` |
| `loopback-bench` | Release | `net_loopback_bench` |
| `net-bench` | Release | `net_workload_bench` |

`net_loopback_bench` and `net_workload_bench` are `EXCLUDE_FROM_ALL` and Release
only, so `cmake --build --preset release` does *not* build them — ask for them by
preset.

## Where binaries land

Relative to the tree root (`build/debug/`, `build/tsan/`, `build/release/`):

```
app                                # engine smoke test (src/main.cpp)
src/net/exchange_server            # the networked front end
src/net/exchange_cli               # reference binary-protocol client
src/net/net_smoke                  # networking test binary
bench/google/exchange_bench        # Google Benchmark microbenchmarks
bench/loopback/net_loopback_bench  # Release only
bench/net/net_workload_bench       # Release only
flash1_adapter.so                  # Release only, via scripts/run_flash1.sh build
```

So, for example: `./build/debug/src/net/net_smoke`,
`./build/release/bench/google/exchange_bench`.

## Debug vs Release: which to use when

**Develop in Debug.** The sanitizers plus `_GLIBCXX_DEBUG` are what catch the
bugs, and there is no unit-test suite to catch them instead.

**Benchmark in Release, always.** `exchange_bench` sets `-O3 -march=native` on
itself in every config, but it links `engine`, which propagates `debug_options`
as a PUBLIC dependency — so a bench binary built in `build/debug/` is still
sanitized and still carries `_GLIBCXX_DEBUG`. Optimized *and* sanitized numbers
are meaningless. The same applies to the network benches: `net_io` links `engine`
normally and inherits its sanitizers, which is why both bench targets are Release
only.

Two things are deliberately sanitizer-free in *any* config:

- **`flash1_adapter`** compiles `OrderBook.cpp` directly rather than linking
  `engine`, so `debug_options` never reaches it and harness numbers never depend
  on how some tree happens to be configured.
- Do **not** copy that trick into `src/net/`. `net_io` links `engine` normally on
  purpose — compiling `OrderBook.cpp` directly there would mismatch
  `std::vector<Fill>` layouts across the `_GLIBCXX_DEBUG` boundary.

**Run the networking tests in both Debug and TSan.** `net_smoke` must pass under
each; that pair substitutes for a unit-test suite. See [RUNNING.md](RUNNING.md).

## Options

| Variable | Default | Meaning |
|---|---|---|
| `EXCHANGE_SANITIZER` | `address` | Sanitizer set for the Debug config: `address` \| `thread` \| `none`. |
| `EXCHANGE_BUILD_NET` | `ON` | Build `src/net` (needs Boost headers). |

```bash
cmake --preset debug -DEXCHANGE_BUILD_NET=OFF
```

## Formatting

`.clang-format` is `BasedOnStyle: Google` (2-space indent). Run
`clang-format -i` on changed files.

## Troubleshooting

**`Could NOT find Boost`** — CMake 4.4 removed `FindBoost.cmake`, so Boost is
required in CONFIG mode. Install `libboost-dev`, or configure with
`-DEXCHANGE_BUILD_NET=OFF` to skip the networking layer entirely.

**A change to Boost includes triggers a huge rebuild** — the layer is header-only
Boost with `BOOST_{ASIO,BEAST}_SEPARATE_COMPILATION`, and exactly one TU
(`src/net/boost_impl.cpp`) carries the implementation. Adding anything to that
file costs a recompile of all of Asio. Keep it minimal.

**Stale tree / wrong compiler picked up** — delete the tree and re-configure:
`rm -rf build/debug && cmake --preset debug`. The preset pins the generator and
config, so a re-configure over an existing tree is otherwise safe.

**`compile_commands.json`** is a symlink at the repo root pointing at
`build/debug/compile_commands.json`, for clangd. Configure the `debug` tree at
least once or the symlink dangles.
