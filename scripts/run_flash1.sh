#!/usr/bin/env bash
# Build/run the engine adapter against the flash1 matching-engine-benchmark.
#
#   scripts/run_flash1.sh build                  build adapter.so (Release)
#   scripts/run_flash1.sh audit  [scenario]      correctness + book audit
#   scripts/run_flash1.sh perf   [scenario]      timed run (verifies hash too)
#   scripts/run_flash1.sh conformance            pre-flight conformance check
#   scripts/run_flash1.sh explain [scenario]     dump canonical output and
#                                                localize first divergence
#   scripts/run_flash1.sh challenge [extra args] 10 perf + 1 audit run per
#                                                scenario, worst-case result
#
# Scenarios: static | normal | swing-25 | swing-40 | flash-crash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HARNESS_DIR="${REPO_ROOT}/external/matching-engine-benchmark"
BUILD_DIR="${REPO_ROOT}/build-release"
ADAPTER="${BUILD_DIR}/bench/flash1/adapter.so"

require_harness() {
  if [[ ! -x "${HARNESS_DIR}/harness" ]]; then
    echo "error: harness not built — run scripts/fetch_harness.sh first" >&2
    exit 1
  fi
}
require_adapter() {
  if [[ ! -f "${ADAPTER}" ]]; then
    echo "error: adapter not built — run: scripts/run_flash1.sh build" >&2
    exit 1
  fi
}

cmd="${1:-}"
case "${cmd}" in
  build)
    # Dedicated single-config Release tree, deliberately NOT the shared build/.
    # The adapter itself is sanitizer-free in any config (it compiles
    # OrderBook.cpp directly rather than linking engine/debug_options), so this
    # is not about sanitizers — it is about owning the generator and config.
    # build/ is user-controlled: it has been both Ninja Multi-Config and plain
    # Ninja/Debug, and against a single-config tree `--build --config Release`
    # is SILENTLY IGNORED, yielding a Debug adapter at a different path. Perf
    # numbers must not depend on how build/ happens to be configured today.
    cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build "${BUILD_DIR}" --target flash1_adapter
    echo "Adapter: ${ADAPTER}"
    ;;
  audit | perf)
    require_harness
    require_adapter
    scenario="${2:-normal}"
    (cd "${HARNESS_DIR}" &&
      ./harness --engine "${ADAPTER}" --scenario "${scenario}" --mode "${cmd}")
    ;;
  conformance)
    require_harness
    require_adapter
    (cd "${HARNESS_DIR}" && python3 scripts/conformance_check.py "${ADAPTER}")
    ;;
  explain)
    require_harness
    require_adapter
    scenario="${2:-normal}"
    out="$(mktemp /tmp/flash1_candidate.XXXXXX.txt)"
    (cd "${HARNESS_DIR}" &&
      ./harness --engine "${ADAPTER}" --scenario "${scenario}" --mode perf \
        --write-canonical-output "${out}" || true)
    (cd "${HARNESS_DIR}" &&
      python3 scripts/explain_divergence.py reference/canonical_output.txt.gz \
        "${out}")
    ;;
  challenge)
    require_harness
    require_adapter
    shift
    (cd "${HARNESS_DIR}" &&
      python3 scripts/run_challenge.py --engine "${ADAPTER}" "$@")
    ;;
  *)
    sed -n '2,14p' "${BASH_SOURCE[0]}"
    exit 1
    ;;
esac
