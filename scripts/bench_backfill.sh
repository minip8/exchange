#!/usr/bin/env bash
# Re-measure historical commits with the *current* Google Benchmark suite.
#
#   scripts/bench_backfill.sh                 backfill the default commit set
#   scripts/bench_backfill.sh <sha> [<sha>…]  backfill specific commits
#   scripts/bench_backfill.sh --reps N        GB repetitions (default 5)
#   scripts/bench_backfill.sh --flash1-reps N flash1 perf reps/scenario (default 5)
#   scripts/bench_backfill.sh --skip-flash1   Google Benchmark suite only
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
# Range limit: the current bench sources need Symbol (a925d17) and net_protocol
# (560595f), so commits older than those cannot be backfilled — they will fail to
# compile rather than silently measure something else.
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

GB_REPS=5
FLASH1_REPS=5
SKIP_FLASH1=0
SCENARIOS=(static normal swing-25 swing-40 flash-crash)
COMMITS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --reps) GB_REPS="$2"; shift 2 ;;
    --flash1-reps) FLASH1_REPS="$2"; shift 2 ;;
    --skip-flash1) SKIP_FLASH1=1; shift ;;
    -h|--help) sed -n '2,33p' "${BASH_SOURCE[0]}"; exit 0 ;;
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

  # The benchmark sources under test are this tree's, not the historical ones.
  cp "${REPO_ROOT}"/bench/google/*.cpp "${REPO_ROOT}"/bench/google/*.hpp \
     "${REPO_ROOT}"/bench/google/CMakeLists.txt "${TREE}/bench/google/"

  # external/ is gitignored, so a fresh worktree has none. Share the main tree's:
  # the harness is pinned and is the one thing that must NOT vary across commits.
  # This has to happen before configuring — bench/flash1/CMakeLists.txt only
  # declares flash1_adapter if it can see the harness headers at configure time,
  # so a later symlink leaves the target silently missing.
  [[ "${SKIP_FLASH1}" -eq 0 ]] && ln -sfn "${REPO_ROOT}/external" "${TREE}/external"

  # cmake --preset resolves CMakePresets.json from the CWD, so each worktree
  # configures and builds into its own build/release/ and the main tree is
  # untouched.
  (cd "${TREE}" && cmake --preset release >/dev/null && cmake --build --preset bench >/dev/null)

  GB_JSON="${WORK_DIR}/${SHORT}.json"
  "${TREE}/build/release/bench/google/exchange_bench" \
    --benchmark_format=console --benchmark_out_format=json --benchmark_out="${GB_JSON}" \
    --benchmark_repetitions="${GB_REPS}" --benchmark_display_aggregates_only=true

  # --- flash1, using this commit's own adapter (see the header) ---
  FLASH1_ARGS=()
  if [[ "${SKIP_FLASH1}" -eq 0 ]]; then
    (cd "${TREE}" && cmake --build --preset flash1 >/dev/null)

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
echo "backfill complete — records in bench/results/, plots in bench/results/plots/"
