#!/usr/bin/env bash
# Fetch and build the flash1-dev/matching-engine-benchmark harness into
# external/matching-engine-benchmark (gitignored). Idempotent.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HARNESS_URL="https://github.com/flash1-dev/matching-engine-benchmark"
HARNESS_DIR="${REPO_ROOT}/external/matching-engine-benchmark"
# Pinned for reproducible results; bump deliberately.
HARNESS_COMMIT="eb6e89fbc4313f77cea7d424bab14a26093cf552"

# --- preflight ---------------------------------------------------------------
if ! command -v g++ >/dev/null; then
  echo "error: g++ not found (harness needs GCC 14+ or Clang 16+)" >&2
  exit 1
fi
gxx_major="$(g++ -dumpversion | cut -d. -f1)"
if ((gxx_major < 14)); then
  echo "error: g++ ${gxx_major} too old (harness needs GCC 14+)" >&2
  exit 1
fi
if ! echo '#include <boost/version.hpp>' | g++ -x c++ -E - >/dev/null 2>&1; then
  echo "error: Boost headers not found (e.g. apt install libboost-dev)" >&2
  exit 1
fi
if ! python3 -c 'import sys; sys.exit(sys.version_info < (3, 8))'; then
  echo "error: python3 >= 3.8 required" >&2
  exit 1
fi

# --- clone / pin -------------------------------------------------------------
if [[ ! -d "${HARNESS_DIR}/.git" ]]; then
  git clone "${HARNESS_URL}" "${HARNESS_DIR}"
fi
git -C "${HARNESS_DIR}" fetch --quiet origin
git -C "${HARNESS_DIR}" checkout --quiet "${HARNESS_COMMIT}"

# --- build harness + generator + baseline (liquibook, for cross-checks) ------
make -C "${HARNESS_DIR}"

echo
echo "Harness ready at ${HARNESS_DIR} (pinned ${HARNESS_COMMIT})"
echo "Next: scripts/run_flash1.sh build"
