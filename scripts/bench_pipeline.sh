#!/usr/bin/env bash
# Benchmark pipeline: run both benchmark suites, record, and plot.
#
#   scripts/bench_pipeline.sh                 quick run (1 flash1 perf rep/scenario)
#   scripts/bench_pipeline.sh --full          5 reps/scenario, median recorded
#                                             (challenge-style scoring; the official
#                                             run_challenge.py uses 10 — 5 is a
#                                             faster personal-machine compromise)
#   scripts/bench_pipeline.sh --skip-flash1   Google Benchmark suite only
#   scripts/bench_pipeline.sh --plot-only     re-render plots from existing records
#
# Output: one record JSON in bench/results/ (gitignored) + PNGs in
# bench/results/plots/. Plotting runs via uv (deps inline in plot_bench.py).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HARNESS_DIR="${REPO_ROOT}/external/matching-engine-benchmark"
SCENARIOS=(static normal swing-25 swing-40 flash-crash)

MODE=quick
REPS=1
SKIP_FLASH1=0
PLOT_ONLY=0
for arg in "$@"; do
  case "${arg}" in
    --full) MODE=full; REPS=5 ;;
    --skip-flash1) SKIP_FLASH1=1 ;;
    --plot-only) PLOT_ONLY=1 ;;
    *)
      echo "error: unknown flag '${arg}'" >&2
      sed -n '2,13p' "${BASH_SOURCE[0]}" >&2
      exit 1
      ;;
  esac
done

if ! command -v uv >/dev/null; then
  echo "error: uv not found — install with: curl -LsSf https://astral.sh/uv/install.sh | sh" >&2
  exit 1
fi

if [[ "${PLOT_ONLY}" -eq 1 ]]; then
  exec uv run "${REPO_ROOT}/scripts/plot_bench.py"
fi

TS="$(date -u +%Y%m%dT%H%M%SZ)"
SHA="$(git -C "${REPO_ROOT}" rev-parse HEAD)"
DIRTY=false
[[ -n "$(git -C "${REPO_ROOT}" status --porcelain)" ]] && DIRTY=true

# --- Google Benchmark suite (Release; Debug numbers are sanitized garbage) ---
if [[ ! -f "${REPO_ROOT}/build/CMakeCache.txt" ]]; then
  cmake -S "${REPO_ROOT}" -B "${REPO_ROOT}/build" -G "Ninja Multi-Config"
fi
cmake --build "${REPO_ROOT}/build" --config Release --target exchange_bench
GB_JSON="$(mktemp /tmp/exchange_gb.XXXXXX.json)"
trap 'rm -f "${GB_JSON}"' EXIT
"${REPO_ROOT}/build/bench/Release/exchange_bench" \
  --benchmark_format=console --benchmark_out_format=json --benchmark_out="${GB_JSON}"

# --- flash1 harness (perf mode; audit measures correctness, not throughput) ---
FLASH1_RESULTS=()
HARNESS_COMMIT=""
if [[ "${SKIP_FLASH1}" -eq 1 ]]; then
  echo "flash1: skipped (--skip-flash1)"
elif [[ ! -x "${HARNESS_DIR}/harness" ]]; then
  echo "warning: flash1 harness not fetched (scripts/fetch_harness.sh); skipping flash1" >&2
else
  HARNESS_COMMIT="$(git -C "${HARNESS_DIR}" rev-parse HEAD)"
  "${REPO_ROOT}/scripts/run_flash1.sh" build
  for scenario in "${SCENARIOS[@]}"; do
    for rep in $(seq 1 "${REPS}"); do
      echo "--- flash1 perf ${scenario} (rep ${rep}/${REPS}) ---"
      out="$("${REPO_ROOT}/scripts/run_flash1.sh" perf "${scenario}" | tee /dev/stderr)" || true
      rel="$(printf '%s\n' "${out}" | sed -n 's/^Result file: //p' | tail -n1)"
      if [[ -n "${rel}" ]]; then
        FLASH1_RESULTS+=("${HARNESS_DIR}/${rel}")
      else
        echo "warning: no result file from ${scenario} rep ${rep} — rep dropped" >&2
      fi
    done
  done
fi

# --- persist record + render plots ---
uv run "${REPO_ROOT}/scripts/plot_bench.py" \
  --gb-json "${GB_JSON}" \
  --flash1-results ${FLASH1_RESULTS[@]+"${FLASH1_RESULTS[@]}"} \
  --harness-commit "${HARNESS_COMMIT}" \
  --sha "${SHA}" --dirty "${DIRTY}" --timestamp "${TS}" \
  --mode "${MODE}" --reps "${REPS}"
