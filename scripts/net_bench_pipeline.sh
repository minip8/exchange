#!/usr/bin/env bash
# Network benchmark pipeline: replay the flash1 workload over the wire,
# record, and plot.
#
#   scripts/net_bench_pipeline.sh                  quick (200k-order stream)
#   scripts/net_bench_pipeline.sh --full           the canonical 1M-order stream
#                                                  and a wider rate sweep
#   scripts/net_bench_pipeline.sh --scenario S     one scenario (repeatable)
#   scripts/net_bench_pipeline.sh --skip-loopback  skip the egress micro-probe
#   scripts/net_bench_pipeline.sh --plot-only      re-render from existing records
#
# Output: one record JSON in bench/results/net/ (gitignored) + PNGs in
# bench/results/net/plots/. Plotting runs via uv (deps inline in
# plot_net_bench.py).
#
# The order stream is the flash1 one, not a synthetic: same five volatility
# regimes, same messages, same prices. See bench/net/Flash1Workload.hpp for
# exactly what "same" does and does not cover.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HARNESS_DIR="${REPO_ROOT}/external/matching-engine-benchmark"
ALL_SCENARIOS=(static normal swing-25 swing-40 flash-crash)

MODE=quick
COUNT=200000
RATES="100000,250000,500000,1000000,0"
SCALING="1,2,4"
SCENARIOS=()
SKIP_LOOPBACK=0
PLOT_ONLY=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --full)
      MODE=full
      COUNT=1000000
      RATES="100000,250000,500000,1000000,2000000,0"
      ;;
    --scenario) SCENARIOS+=("$2"); shift ;;
    --skip-loopback) SKIP_LOOPBACK=1 ;;
    --plot-only) PLOT_ONLY=1 ;;
    *)
      echo "error: unknown flag '$1'" >&2
      sed -n '2,15p' "${BASH_SOURCE[0]}" >&2
      exit 1
      ;;
  esac
  shift
done
[[ ${#SCENARIOS[@]} -eq 0 ]] && SCENARIOS=("${ALL_SCENARIOS[@]}")

if ! command -v uv >/dev/null; then
  echo "error: uv not found — install with: curl -LsSf https://astral.sh/uv/install.sh | sh" >&2
  exit 1
fi

if [[ "${PLOT_ONLY}" -eq 1 ]]; then
  exec uv run "${REPO_ROOT}/scripts/plot_net_bench.py"
fi

# The workload files live with the harness. Building the bench does not need
# them; running it does, and generating one takes ~15s per scenario.
if [[ ! -x "${HARNESS_DIR}/generator" ]]; then
  echo "error: ${HARNESS_DIR}/generator not found — run scripts/fetch_harness.sh first" >&2
  exit 1
fi

TS="$(date -u +%Y%m%dT%H%M%SZ)"
SHA="$(git -C "${REPO_ROOT}" rev-parse HEAD)"
DIRTY=false
[[ -n "$(git -C "${REPO_ROOT}" status --porcelain)" ]] && DIRTY=true
HARNESS_COMMIT="$(git -C "${HARNESS_DIR}" rev-parse HEAD)"

# Release only: net_io links `engine`, which propagates debug_options, so a
# Debug build of either bench is sanitized and its numbers are meaningless.
# cmake --preset resolves CMakePresets.json from the CWD, hence the cd.
(cd "${REPO_ROOT}" && cmake --preset release && cmake --build --preset net-bench)

NET_JSON="$(mktemp /tmp/exchange_net.XXXXXX.json)"
LOOPBACK_JSON="$(mktemp /tmp/exchange_loopback.XXXXXX.json)"
trap 'rm -f "${NET_JSON}" "${LOOPBACK_JSON}"' EXIT

SCENARIO_ARGS=()
for scenario in "${SCENARIOS[@]}"; do
  SCENARIO_ARGS+=(--scenario "${scenario}")
done

# spin-us 200 keeps the matching thread hot, so the numbers measure the
# transport rather than the idle-wake policy.
(cd "${REPO_ROOT}" && ./build/release/bench/net/net_workload_bench \
    --mode both \
    "${SCENARIO_ARGS[@]}" \
    --count "${COUNT}" \
    --rates "${RATES}" \
    --scaling "${SCALING}" \
    --spin-us 200 \
    --json "${NET_JSON}")

# --- egress micro-probe (a different question: what does one wake carry?) ---
LOOPBACK_ARG=()
if [[ "${SKIP_LOOPBACK}" -eq 1 ]]; then
  echo "loopback: skipped (--skip-loopback)"
else
  echo "--- egress micro-probe ---"
  (cd "${REPO_ROOT}" && cmake --build --preset loopback-bench)
  (cd "${REPO_ROOT}" && ./build/release/bench/loopback/net_loopback_bench \
      --spin-us 200 --json "${LOOPBACK_JSON}")
  LOOPBACK_ARG=(--loopback-json "${LOOPBACK_JSON}")
fi

# --- persist record + render plots ---
uv run "${REPO_ROOT}/scripts/plot_net_bench.py" \
  --net-json "${NET_JSON}" \
  ${LOOPBACK_ARG[@]+"${LOOPBACK_ARG[@]}"} \
  --harness-commit "${HARNESS_COMMIT}" \
  --sha "${SHA}" --dirty "${DIRTY}" --timestamp "${TS}" \
  --mode "${MODE}"
