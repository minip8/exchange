#!/usr/bin/env bash
# Re-measure historical commits with the *current* Google Benchmark suite.
#
#   scripts/bench_backfill.sh                 backfill the default commit set
#   scripts/bench_backfill.sh <sha> [<sha>…]  backfill specific commits
#   scripts/bench_backfill.sh --flash1-reps N flash1 perf reps/scenario (default 5)
#   scripts/bench_backfill.sh --skip-flash1   Google Benchmark suite only
#
# exchange_bench always runs at --benchmark_repetitions=1; only the flash1 rep
# count is tunable.
#
# Why this exists: bench_pipeline.sh always records HEAD at wall-clock now, so it
# cannot express "these are commit X's numbers". plot_bench.py can — it takes
# --sha and --timestamp — so this drives that directly.
#
# The two suites are backfilled on opposite rules, and the difference is not
# arbitrary:
#
#   bench/google/ is copied in from *this* tree. The benchmark code is the
#   measuring instrument, so it must be identical at every point or the trend is
#   comparing two instruments and calling the difference a performance change.
#
#   bench/flash1/ is left as each commit found it. There the instrument is the
#   external harness (pinned in external/, shared via a symlink below), and the
#   adapter is part of what is being measured — it is coupled to the engine's
#   layout. 586ecc6 reversed the level ordering and had to change adapter.cpp in
#   the same commit; pinning HEAD's adapter onto an older engine would read the
#   worst price as the best one and the run would be wrong, not merely stale.
#
# Range limit, and why there are two build paths:
#
#   bench_matching_engine.cpp needs Symbol (a925d17), bench_ring.cpp needs
#   net_protocol (560595f), and bench_flash1_replay.cpp needs both. Those three
#   cannot be built against an older engine at all.
#
#   bench_orderbook.cpp can. The engine API it uses — addOrder, removeOrder,
#   modifyOrder, contains, buys()/sells() — is unchanged all the way back to
#   612785f; only book *construction* differs, which BenchSupport.hpp handles
#   with a __has_include guard. So pre-preset commits get a reduced,
#   OrderBook-only suite: 9 benchmarks covering every curated HISTORY_LATENCY_NS
#   metric plus BM_MixedWorkload_SteadyState. Only
#   BM_Engine_MultiBook_SteadyState is missing from those points on the trend.
#
# A tree is "legacy" iff it has no CMakePresets.json. Those trees also have a
# flat bench/ with no google/ subdirectory, and their bench/CMakeLists.txt
# already pins googlebenchmark v1.9.5, links benchmark::benchmark_main and sets
# -O3 -march=native — identical to HEAD's — so it is left untouched and only the
# sources are swapped. Their debug_options gates every sanitizer behind
# $<$<CONFIG:Debug>>, so a plain -DCMAKE_BUILD_TYPE=Release build is
# sanitizer-free, exactly as the release preset is.
#
# Run this on an idle machine in one sitting. The whole point is cross-run
# comparability, and thermal or scheduler drift between commits defeats it.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK_DIR="${REPO_ROOT}/build/backfill"

# Default set: every commit that changed engine code since the bench sources
# stabilised, plus HEAD.
#
# 586ecc6 and HEAD have byte-identical engine source (cb5c9ca and its revert
# fbd2446 cancel out), so the gap between those two points is a measured noise
# floor — the yardstick for judging whether any other step here is real.
DEFAULT_COMMITS=(f8c5fb6 c842652 2e0115e 586ecc6 cb5c9ca HEAD)

FLASH1_REPS=5
SKIP_FLASH1=0
SCENARIOS=(static normal swing-25 swing-40 flash-crash)
COMMITS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --flash1-reps) FLASH1_REPS="$2"; shift 2 ;;
    --skip-flash1) SKIP_FLASH1=1; shift ;;
    -h|--help) sed -n '2,35p' "${BASH_SOURCE[0]}"; exit 0 ;;
    -*) echo "error: unknown flag '$1'" >&2; exit 1 ;;
    *) COMMITS+=("$1"); shift ;;
  esac
done
[[ ${#COMMITS[@]} -eq 0 ]] && COMMITS=("${DEFAULT_COMMITS[@]}")

HARNESS_DIR="${REPO_ROOT}/external/matching-engine-benchmark"
if [[ "${SKIP_FLASH1}" -eq 0 && ! -x "${HARNESS_DIR}/harness" ]]; then
  echo "error: flash1 harness not fetched — run scripts/fetch_harness.sh, or pass --skip-flash1" >&2
  exit 1
fi

if ! command -v uv >/dev/null; then
  echo "error: uv not found — install with: curl -LsSf https://astral.sh/uv/install.sh | sh" >&2
  exit 1
fi

mkdir -p "${WORK_DIR}"

HARNESS_COMMIT=""
[[ "${SKIP_FLASH1}" -eq 0 ]] && HARNESS_COMMIT="$(git -C "${HARNESS_DIR}" rev-parse HEAD)"

for ref in "${COMMITS[@]}"; do
  SHA="$(git -C "${REPO_ROOT}" rev-parse "${ref}")"
  SHORT="${SHA:0:7}"
  # The timestamp is the *commit* date, not now: load_history sorts records by
  # timestamp_utc, so backfilled points must sort in commit order or the trend
  # chart's x-axis is meaningless.
  # format-local under TZ=UTC, so the recorded timestamp really is UTC — plain
  # `format:` would emit the commit's own offset with a Z suffix bolted on.
  TS="$(TZ=UTC git -C "${REPO_ROOT}" log -1 --format=%cd \
        --date=format-local:%Y%m%dT%H%M%SZ "${SHA}")"
  TREE="${WORK_DIR}/${SHORT}"

  echo "=== ${SHORT} (${TS})  $(git -C "${REPO_ROOT}" log -1 --format=%s "${SHA}") ==="

  rm -rf "${TREE}"
  git -C "${REPO_ROOT}" worktree prune
  git -C "${REPO_ROOT}" worktree add --detach "${TREE}" "${SHA}" >/dev/null

  LEGACY=0
  [[ -f "${TREE}/CMakePresets.json" ]] || LEGACY=1

  # The benchmark sources under test are this tree's, not the historical ones.
  if [[ "${LEGACY}" -eq 1 ]]; then
    # Flat bench/, and only the two files that can compile here. The one source
    # already named in its add_executable that cannot is blanked rather than
    # removed, so bench/CMakeLists.txt needs no edit at all.
    cp "${REPO_ROOT}"/bench/google/BenchSupport.hpp \
       "${REPO_ROOT}"/bench/google/bench_orderbook.cpp "${TREE}/bench/"
    : > "${TREE}/bench/bench_matching_engine.cpp"
  else
    cp "${REPO_ROOT}"/bench/google/*.cpp "${REPO_ROOT}"/bench/google/*.hpp \
       "${REPO_ROOT}"/bench/google/CMakeLists.txt "${TREE}/bench/google/"
  fi

  # external/ is gitignored, so a fresh worktree has none. Share the main tree's:
  # the harness is pinned and is the one thing that must NOT vary across commits.
  # This has to happen before configuring — bench/flash1/CMakeLists.txt only
  # declares flash1_adapter if it can see the harness headers at configure time,
  # so a later symlink leaves the target silently missing.
  [[ "${SKIP_FLASH1}" -eq 0 ]] && ln -sfn "${REPO_ROOT}/external" "${TREE}/external"

  if [[ "${LEGACY}" -eq 1 ]]; then
    # build-release/ is where this commit's own run_flash1.sh looks, so the two
    # builds share one configured tree.
    cmake -S "${TREE}" -B "${TREE}/build-release" -G Ninja \
          -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "${TREE}/build-release" --target exchange_bench >/dev/null
    BENCH_BIN="${TREE}/build-release/bench/exchange_bench"
  else
    # cmake --preset resolves CMakePresets.json from the CWD, so each worktree
    # configures and builds into its own build/release/ and the main tree is
    # untouched.
    (cd "${TREE}" && cmake --preset release >/dev/null && cmake --build --preset bench >/dev/null)
    BENCH_BIN="${TREE}/build/release/bench/google/exchange_bench"
  fi

  GB_JSON="${WORK_DIR}/${SHORT}.json"
  # One repetition, so the JSON carries a single `iteration` row per benchmark
  # and no aggregates. --benchmark_display_aggregates_only is deliberately
  # absent — with one repetition there are no aggregates for it to display.
  #
  # The negative filter matches bench_pipeline.sh: BM_Flash1_ replays a 2M-message
  # stream and reports instrumented times, so it does not belong in the record
  # being trended. (Legacy trees never build it, and the filter is a no-op there.)
  "${BENCH_BIN}" \
    --benchmark_filter='-^BM_Flash1_' \
    --benchmark_format=console --benchmark_out_format=json --benchmark_out="${GB_JSON}" \
    --benchmark_repetitions=1

  # --- flash1, using this commit's own adapter (see the header) ---
  FLASH1_ARGS=()
  if [[ "${SKIP_FLASH1}" -eq 0 ]]; then
    if [[ "${LEGACY}" -eq 1 ]]; then
      # No flash1 preset that far back; the tree's own run_flash1.sh knows how to
      # build its adapter, into the build-release/ configured above.
      "${TREE}/scripts/run_flash1.sh" build >/dev/null
    else
      (cd "${TREE}" && cmake --build --preset flash1 >/dev/null)
    fi

    for scenario in "${SCENARIOS[@]}"; do
      for rep in $(seq 1 "${FLASH1_REPS}"); do
        out="$("${TREE}/scripts/run_flash1.sh" perf "${scenario}")" || true
        rel="$(printf '%s\n' "${out}" | sed -n 's/^Result file: //p' | tail -n1)"
        if [[ -n "${rel}" ]]; then
          FLASH1_ARGS+=("${HARNESS_DIR}/${rel}")
        else
          echo "warning: no result file from ${scenario} rep ${rep} — rep dropped" >&2
        fi
      done
      echo "  flash1 ${scenario}: ${FLASH1_REPS} reps"
    done
  fi

  PLOT_ARGS=(--gb-json "${GB_JSON}" --sha "${SHA}" --timestamp "${TS}" --mode quick)
  if [[ ${#FLASH1_ARGS[@]} -gt 0 ]]; then
    PLOT_ARGS+=(--reps "${FLASH1_REPS}" --harness-commit "${HARNESS_COMMIT}"
                --flash1-results "${FLASH1_ARGS[@]}")
  fi
  (cd "${REPO_ROOT}" && uv run scripts/plot_bench.py "${PLOT_ARGS[@]}")

  git -C "${REPO_ROOT}" worktree remove --force "${TREE}"
done

echo
echo "backfill complete — records and per-commit plots in bench/results/<shortsha>/,"
echo "trend charts in bench/results/plots/"
