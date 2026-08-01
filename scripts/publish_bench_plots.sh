#!/usr/bin/env bash
# Copy per-commit benchmark plots out of gitignored bench/results/ into tracked
# docs/bench/, which is what README.md embeds.
#
#   scripts/publish_bench_plots.sh                 publish the README commit set
#   scripts/publish_bench_plots.sh <sha> [<sha>…]  publish specific commits
#
# A copy step rather than un-ignoring bench/results/, on purpose: publishing is
# then an explicit act, so a scratch run or a dirty-tree experiment never lands
# in git just because it happened to be the last thing measured.
#
# Also prints the markdown block for each commit, since the numbers in README.md
# have to agree with the plots sitting beside them.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RESULTS="${REPO_ROOT}/bench/results"
DOCS="${REPO_ROOT}/docs/bench"

# The commits README.md tells the story with. Keep in sync with its
# "## Benchmarks (OrderBook)" section.
DEFAULT_COMMITS=(612785f 28895d3 83bd5ae f8c5fb6 586ecc6 cb5c9ca)

COMMITS=("$@")
[[ ${#COMMITS[@]} -eq 0 ]] && COMMITS=("${DEFAULT_COMMITS[@]}")

if ! command -v uv >/dev/null; then
  echo "error: uv not found — needed to read the records" >&2
  exit 1
fi

for sha in "${COMMITS[@]}"; do
  src="${RESULTS}/${sha}/plots"
  if [[ ! -d "${src}" ]]; then
    echo "error: no plots for ${sha} — run scripts/bench_backfill.sh ${sha}" >&2
    exit 1
  fi
  dest="${DOCS}/${sha}"
  mkdir -p "${dest}"
  # Only the two the README embeds. flash1_latency.png is a HEAD-only artifact
  # (bench_backfill.sh does not pass --hist-json) and would be a broken link.
  copied=()
  for stem in flash1 gb; do
    if [[ -f "${src}/${stem}.png" ]]; then
      cp "${src}/${stem}.png" "${dest}/${stem}.png"
      copied+=("${stem}.png")
    fi
  done
  if [[ ${#copied[@]} -eq 0 ]]; then
    echo "error: ${src} has neither flash1.png nor gb.png" >&2
    exit 1
  fi
  echo "${sha}: ${copied[*]} -> docs/bench/${sha}/"
done

echo
echo "=== markdown ==="
uv run "${REPO_ROOT}/scripts/bench_summary.py" "${COMMITS[@]}"
