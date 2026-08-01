#!/usr/bin/env bash
# Benchmark pipeline: run both benchmark suites, record, and plot.
#
#   scripts/bench_pipeline.sh                 quick run (5 flash1 perf reps/scenario)
#   scripts/bench_pipeline.sh --full          10 flash1 reps/scenario
#                                             (challenge-style scoring — 10 reps is
#                                             what the official run_challenge.py uses)
#   scripts/bench_pipeline.sh --skip-flash1   skip the flash1 harness perf runs
#   scripts/bench_pipeline.sh --skip-hist     skip the flash1 per-op latency
#                                             distribution (independent of
#                                             --skip-flash1; pass both for the
#                                             microbenchmarks alone)
#   scripts/bench_pipeline.sh --net           also run the network latency bench
#   scripts/bench_pipeline.sh --plot-only     re-render plots from existing records
#
# exchange_bench always runs at --benchmark_repetitions=1, in both modes; only
# the flash1 rep count varies.
#
# exchange_bench is invoked TWICE, on purpose. The default suite excludes the
# flash1 replay benchmarks, and a second filtered run does only those — they
# replay a 2M-message stream and would otherwise add minutes to, and share a
# process with, the microbenchmarks whose numbers this pipeline trends.
#
# Output: one record JSON plus this commit's own plots in
# bench/results/<shortsha>/ (gitignored), and the cross-commit trend charts in
# bench/results/plots/. Plotting runs via uv (deps inline in plot_bench.py).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HARNESS_DIR="${REPO_ROOT}/external/matching-engine-benchmark"
SCENARIOS=(static normal swing-25 swing-40 flash-crash)

MODE=quick
# flash1 perf reps per scenario. A rep costs ~4s against a suite that already
# runs for minutes, so there is no reason to record a single unrepeated sample.
# --full matches run_challenge.py's 10.
REPS=5
SKIP_FLASH1=0
SKIP_HIST=0
PLOT_ONLY=0
RUN_NET=0
for arg in "$@"; do
  case "${arg}" in
    --full) MODE=full; REPS=10 ;;
    --skip-flash1) SKIP_FLASH1=1 ;;
    --skip-hist) SKIP_HIST=1 ;;
    --net) RUN_NET=1 ;;
    --plot-only) PLOT_ONLY=1 ;;
    *)
      echo "error: unknown flag '${arg}'" >&2
      sed -n '2,25p' "${BASH_SOURCE[0]}" >&2
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
# Configure unconditionally: the release preset pins generator and config, so
# this cannot inherit a stale tree and silently benchmark the wrong binary.
# cmake --preset resolves CMakePresets.json from the CWD, hence the cd.
(cd "${REPO_ROOT}" && cmake --preset release && cmake --build --preset bench)
GB_JSON="$(mktemp /tmp/exchange_gb.XXXXXX.json)"
HIST_JSON="$(mktemp /tmp/exchange_hist.XXXXXX.json)"
# One trap, both files: a second `trap ... EXIT` silently REPLACES the first.
trap 'rm -f "${GB_JSON}" "${HIST_JSON}"' EXIT
# One repetition: the record carries a single `iteration` row per benchmark and
# no aggregates, which is the shape plot_bench.py falls back to (gb_entries) and
# which leaves gb_stddev returning None, so the trend chart draws no spread band.
# --benchmark_display_aggregates_only is deliberately absent — with one
# repetition there are no aggregates for it to display.
#
# The leading '-' is Google Benchmark's negative filter: this run is everything
# EXCEPT the flash1 replay, which the second invocation below owns.
"${REPO_ROOT}/build/release/bench/google/exchange_bench" \
  --benchmark_filter='-^BM_Flash1_' \
  --benchmark_format=console --benchmark_out_format=json --benchmark_out="${GB_JSON}" \
  --benchmark_repetitions=1

# --- flash1 per-operation latency distribution (the other exchange_bench run) ---
# Its Google Benchmark JSON is deliberately discarded: what this run produces is
# the sidecar histogram, and folding a 2M-message replay into the microbenchmark
# record would put it in the bar chart next to 40 ns fixtures.
HIST_ARG=()
if [[ "${SKIP_HIST}" -eq 1 ]]; then
  echo "latency distribution: skipped (--skip-hist)"
elif [[ ! -x "${HARNESS_DIR}/generator" ]]; then
  echo "warning: flash1 harness not fetched (scripts/fetch_harness.sh); skipping latency distribution" >&2
else
  echo "--- flash1 per-operation latency distribution ---"
  "${REPO_ROOT}/build/release/bench/google/exchange_bench" \
    --benchmark_filter='^BM_Flash1_' \
    --benchmark_format=console \
    --benchmark_repetitions=1 \
    --harness-dir "${HARNESS_DIR}" \
    --hist-json "${HIST_JSON}"
  HIST_ARG=(--hist-json "${HIST_JSON}")
fi

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

# --- network latency (opt-in: it takes ~30s and needs no harness) ---
#
# Not folded into the recorded JSON. Its output is a latency distribution
# across three pipeline depths and two egress implementations, which does not
# fit the single-number-per-scenario shape the plots are built around, and
# flattening it to a median would throw away the only interesting part.
if [[ "${RUN_NET}" -eq 1 ]]; then
  echo "--- network latency ---"
  (cd "${REPO_ROOT}" && cmake --build --preset loopback-bench)
  # spin-us 200 keeps the matching thread hot, so the numbers measure the
  # transport rather than the idle-wake policy.
  (cd "${REPO_ROOT}" && ./build/release/bench/loopback/net_loopback_bench \
      --spin-us 200)
fi

# --- persist record + render plots ---
uv run "${REPO_ROOT}/scripts/plot_bench.py" \
  --gb-json "${GB_JSON}" \
  --flash1-results ${FLASH1_RESULTS[@]+"${FLASH1_RESULTS[@]}"} \
  ${HIST_ARG[@]+"${HIST_ARG[@]}"} \
  --harness-commit "${HARNESS_COMMIT}" \
  --sha "${SHA}" --dirty "${DIRTY}" --timestamp "${TS}" \
  --mode "${MODE}" --reps "${REPS}"
