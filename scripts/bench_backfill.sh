#!/usr/bin/env bash
# Re-measure historical commits with the *current* Google Benchmark suite.
#
#   scripts/bench_backfill.sh                 backfill the default commit set
#   scripts/bench_backfill.sh <sha> [<sha>…]  backfill specific commits
#   scripts/bench_backfill.sh --reps N        repetitions per benchmark (default 5)
#
# Why this exists: bench_pipeline.sh always records HEAD at wall-clock now, so it
# cannot express "these are commit X's numbers". plot_bench.py can — it takes
# --sha and --timestamp — so this drives that directly.
#
# Each commit is built in its own git worktree with this tree's bench/google/
# sources copied in, so every point on the resulting trend is the same benchmark
# code measuring different engine code. That is the only way the trend means
# anything.
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
COMMITS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --reps) GB_REPS="$2"; shift 2 ;;
    -h|--help) sed -n '2,22p' "${BASH_SOURCE[0]}"; exit 0 ;;
    -*) echo "error: unknown flag '$1'" >&2; exit 1 ;;
    *) COMMITS+=("$1"); shift ;;
  esac
done
[[ ${#COMMITS[@]} -eq 0 ]] && COMMITS=("${DEFAULT_COMMITS[@]}")

if ! command -v uv >/dev/null; then
  echo "error: uv not found — install with: curl -LsSf https://astral.sh/uv/install.sh | sh" >&2
  exit 1
fi

mkdir -p "${WORK_DIR}"

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

  # cmake --preset resolves CMakePresets.json from the CWD, so each worktree
  # configures and builds into its own build/release/ and the main tree is
  # untouched.
  (cd "${TREE}" && cmake --preset release >/dev/null && cmake --build --preset bench >/dev/null)

  GB_JSON="${WORK_DIR}/${SHORT}.json"
  "${TREE}/build/release/bench/google/exchange_bench" \
    --benchmark_format=console --benchmark_out_format=json --benchmark_out="${GB_JSON}" \
    --benchmark_repetitions="${GB_REPS}" --benchmark_display_aggregates_only=true

  # flash1 is deliberately not re-run: it does not use Google Benchmark, is
  # unaffected by this work, and its numbers for these commits are already in
  # the archived records.
  (cd "${REPO_ROOT}" && uv run scripts/plot_bench.py \
      --gb-json "${GB_JSON}" --sha "${SHA}" --timestamp "${TS}" --mode quick)

  git -C "${REPO_ROOT}" worktree remove --force "${TREE}"
done

echo
echo "backfill complete — records in bench/results/, plots in bench/results/plots/"
