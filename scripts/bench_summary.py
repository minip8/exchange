#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# ///
"""Print the README markdown block for one or more measured commits.

Reads the newest record in bench/results/<sha>/ and renders the numbers exactly
as README.md carries them, so the prose and the plots beside it cannot drift
apart. Invoked by scripts/publish_bench_plots.sh; also useful on its own.

    scripts/bench_summary.py 586ecc6 cb5c9ca
"""

import json
import sys
from pathlib import Path

RESULTS = Path(__file__).resolve().parent.parent / "bench" / "results"
SCENARIOS = ["static", "normal", "swing-25", "swing-40", "flash-crash"]

# The microbenchmarks worth naming in prose: the two ends of the level scan (the
# pair that makes a layout change legible) plus the one end-to-end throughput
# number. Deliberately a subset of plot_bench.py's curated metrics — the rest are
# in the plot.
KEY_LATENCY = [
    ("BM_AddRemove_BestPrice_AtDepth/1024", "best-price insert @1024"),
    ("BM_AddRemove_WorstPrice_AtDepth/1024", "worst-price insert @1024"),
]
KEY_THROUGHPUT = ("BM_MixedWorkload_SteadyState", "mixed steady-state")

_TIME_UNIT_TO_NS = {"ns": 1.0, "us": 1e3, "ms": 1e6, "s": 1e9}


def newest_record(sha):
    paths = sorted((RESULTS / sha).glob("run_*.json"), reverse=True)
    if not paths:
        sys.exit(f"error: no record in bench/results/{sha}/")
    return json.loads(paths[0].read_text())


def gb_rows(record):
    return (record.get("google_benchmark") or {}).get("benchmarks", [])


def gb_lookup(record, name):
    for b in gb_rows(record):
        if b.get("name") == name and b.get("run_type", "iteration") == "iteration":
            return b
    return None


def flash1_line(record):
    f1 = record.get("flash1")
    if not f1:
        return None
    parts = []
    for sc in SCENARIOS:
        if sc not in f1["scenarios"]:
            continue
        mps = f1["scenarios"][sc]["mps_median"]
        # The score is the worst case, so that is the one in bold.
        parts.append(f"**{sc} {mps:.2f}**" if sc == f1["worst_scenario"] else f"{sc} {mps:.2f}")
    suffix = "" if f1["all_valid"] else "  ** ONE OR MORE RUNS INVALID **"
    return f"flash1 M msgs/s — {' · '.join(parts)} (worst case){suffix}"


def latency_line(record):
    """The uninstrumented per-op cost, from the `_NoTiming` twins.

    Deliberately the twin and not the instrumented row: the fences the timed
    version needs cost ~10-90% and also forbid cross-operation overlap, so the
    instrumented ns/op is the instrument's number, not the engine's. This is
    also engine-only, where the flash1 M msgs/s above goes through the harness.
    """
    lat = record.get("flash1_latency")
    if not lat:
        return None
    raw = {r["scenario"]: r.get("ns_per_op")
           for r in lat.get("runs", []) if not r.get("instrumented")}
    parts = [f"{sc} {raw[sc]:.0f}" for sc in SCENARIOS if raw.get(sc)]
    if not parts:
        return None
    unit = "ns" if (lat.get("timer") or {}).get("units") == "ns" else "TSC ticks"
    return f"per-op {unit} (uninstrumented) — {' · '.join(parts)}"


def gb_line(record):
    parts = []
    for name, label in KEY_LATENCY:
        b = gb_lookup(record, name)
        if b:
            ns = b["real_time"] * _TIME_UNIT_TO_NS.get(b.get("time_unit", "ns"), 1.0)
            parts.append(f"{label} **{ns:,.0f} ns**")
    name, label = KEY_THROUGHPUT
    b = gb_lookup(record, name)
    if b and "items_per_second" in b:
        parts.append(f"{label} **{b['items_per_second'] / 1e6:.1f} M/s**")
    return " · ".join(parts) if parts else None


def main(argv):
    if not argv:
        sys.exit(__doc__)
    for sha in argv:
        record = newest_record(sha)
        print(f"### {sha}\n")
        for line in (flash1_line(record), latency_line(record), gb_line(record)):
            if line:
                print(f"{line}\n")
        for stem, alt in (("flash1", "flash1 throughput"), ("gb", "microbenchmarks"),
                          ("flash1_latency", "per-operation latency distribution")):
            if (RESULTS / sha / "plots" / f"{stem}.png").exists():
                print(f"![{alt}](docs/bench/{sha}/{stem}.png)")
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
