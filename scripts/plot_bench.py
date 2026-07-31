#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = ["matplotlib"]
# ///
"""Assemble a benchmark run record and render plots.

Invoked by scripts/bench_pipeline.sh with fresh Google Benchmark JSON and
flash1 harness result files; also runnable standalone without --gb-json to
re-plot from the existing history in bench/results/.

One self-contained JSON record per pipeline run:
  bench/results/run_<UTCts>_<shortsha>[-dirty].json
Plots (regenerated from full history every run):
  bench/results/plots/{flash1_latest,flash1_history,gb_latest,gb_history,
                       flash1_latency}.png
"""

import argparse
import json
import math
import os
import statistics
import sys
from datetime import datetime, timezone
from pathlib import Path

SCHEMA_VERSION = 1

# Canonical scenario order; also the fixed color-slot assignment.
SCENARIOS = ["static", "normal", "swing-25", "swing-40", "flash-crash"]

# Baseline from CLAUDE.md (WSL2, July 2026) — update there first, then here.
# Deliberately kept low: this is an early-history snapshot, not a current
# measurement, so the +N% deltas printed against it routinely run into the
# thousands of percent. That is expected and is not a sign of a broken run —
# the correctness gate is the per-scenario VALID verdict, not the delta.
BASELINE_MPS = {
    "static": 1.12,
    "normal": 1.91,
    "swing-25": 0.16,
    "swing-40": 0.092,
    "flash-crash": 0.059,
}

# Curated key metrics for the history trend chart. Kept to <= len(SERIES) names
# per panel so each gets its own colour slot.
HISTORY_THROUGHPUT = ["BM_MixedWorkload_SteadyState", "BM_Engine_MultiBook_SteadyState/100"]
HISTORY_LATENCY_NS = [
    # The two ends of the level scan: worst-price is the O(depth) case, best-price
    # is the O(1) one the worst-first layout exists to produce. Tracking both is
    # what makes a layout change legible rather than just "some number moved".
    "BM_AddRemove_WorstPrice_AtDepth/1024",
    "BM_AddRemove_BestPrice_AtDepth/1024",
    # The two intra-level axes — linear find/erase within a PriceLevel, and the
    # front-erase a sweep leaves behind.
    "BM_AddRemove_WithinLevel/256",
    "BM_AddOrder_Match_WithinLevel/64",
    "BM_AddOrder_Match_SweepLevels/64",
]

# Light-mode reference palette (validated fixed order — do not re-order).
SERIES = ["#2a78d6", "#008300", "#e87ba4", "#eda100", "#1baf7a"]
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK2 = "#52514e"
MUTED = "#898781"
GRID = "#e1e0d9"
AXIS = "#c3c2b7"
CRITICAL = "#d03b3b"

_TIME_UNIT_TO_NS = {"ns": 1.0, "us": 1e3, "ms": 1e6, "s": 1e9}


def parse_args(argv):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--gb-json", type=Path, help="fresh Google Benchmark JSON; omit to only re-plot history")
    p.add_argument("--flash1-results", type=Path, nargs="*", default=[], help="harness result JSONs for this run")
    p.add_argument("--hist-json", type=Path, default=None,
                   help="exchange_bench --hist-json sidecar: the flash1 per-op latency distribution")
    p.add_argument("--harness-commit", default=None)
    p.add_argument("--sha", default=None)
    p.add_argument("--dirty", choices=["true", "false"], default="false")
    p.add_argument("--timestamp", default=None, help="compact UTC, e.g. 20260718T051203Z")
    p.add_argument("--mode", choices=["quick", "full"], default="quick")
    p.add_argument("--reps", type=int, default=1)
    p.add_argument("--out-dir", type=Path, default=Path(__file__).resolve().parent.parent / "bench" / "results")
    p.add_argument("--svg", action="store_true", help="also write SVG next to each PNG")
    args = p.parse_args(argv)
    if args.gb_json and not (args.sha and args.timestamp):
        p.error("--gb-json requires --sha and --timestamp")
    return args


# ---------------------------------------------------------------- record I/O


def assemble_record(args):
    gb = json.loads(args.gb_json.read_text())

    flash1 = None
    raws = []
    for path in args.flash1_results:
        try:
            raws.append(json.loads(path.read_text()))
        except (OSError, json.JSONDecodeError) as e:
            print(f"warning: skipping unreadable flash1 result {path}: {e}", file=sys.stderr)
    if raws:
        scenarios = {}
        for raw in raws:
            s = scenarios.setdefault(raw["scenario"], {"mps": [], "verdicts": [], "raw": []})
            s["mps"].append(raw["throughput_msgs_per_s"] / 1e6)
            s["verdicts"].append(raw["verdict"])
            s["raw"].append(raw)
        for s in scenarios.values():
            s["mps_median"] = statistics.median(s["mps"])
        worst = min(scenarios, key=lambda k: scenarios[k]["mps_median"])
        flash1 = {
            "harness_commit": args.harness_commit,
            "reps_per_scenario": args.reps,
            "scenarios": scenarios,
            "worst_case_mps": scenarios[worst]["mps_median"],
            "worst_scenario": worst,
            "all_valid": all(v == "VALID" for s in scenarios.values() for v in s["verdicts"]),
        }

    # Additive and optional: records written before this existed simply lack the
    # key, which is why SCHEMA_VERSION does NOT move — load_history() drops every
    # record whose schema differs, so bumping it would silently erase the whole
    # trend history. Every reader below uses .get().
    flash1_latency = None
    if args.hist_json:
        try:
            flash1_latency = json.loads(args.hist_json.read_text())
        except (OSError, json.JSONDecodeError) as e:
            print(f"warning: skipping unreadable hist json {args.hist_json}: {e}", file=sys.stderr)

    ts = datetime.strptime(args.timestamp, "%Y%m%dT%H%M%SZ").replace(tzinfo=timezone.utc)
    return {
        "schema": SCHEMA_VERSION,
        "timestamp_utc": ts.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "git_sha": args.sha,
        "git_sha_short": args.sha[:7],
        "git_dirty": args.dirty == "true",
        "mode": args.mode,
        "google_benchmark": gb,
        "flash1": flash1,
        "flash1_latency": flash1_latency,
    }


def write_record(record, out_dir, timestamp):
    suffix = "-dirty" if record["git_dirty"] else ""
    path = out_dir / f"run_{timestamp}_{record['git_sha_short']}{suffix}.json"
    tmp = path.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(record, indent=2) + "\n")
    os.replace(tmp, path)
    return path


def load_history(out_dir):
    records = []
    for path in sorted(out_dir.glob("run_*.json")):
        try:
            rec = json.loads(path.read_text())
        except (OSError, json.JSONDecodeError) as e:
            print(f"warning: skipping unreadable record {path}: {e}", file=sys.stderr)
            continue
        if rec.get("schema") == SCHEMA_VERSION:
            records.append(rec)
    records.sort(key=lambda r: r["timestamp_utc"])
    return records


# ------------------------------------------------------------- GB extraction


def gb_entries(record):
    """One entry per benchmark name: the median across repetitions if the run
    had any, else the single iteration.

    Runs recorded before repetitions were added carry only `iteration` rows, so
    the fallback is what keeps those records loadable. Aggregate rows carry the
    repetition count in `name` suffixes (`X_median`), so they are re-keyed back
    to the plain benchmark name here and nowhere else needs to know.
    """
    benchmarks = record["google_benchmark"].get("benchmarks", [])
    medians = {
        b["run_name"]: b
        for b in benchmarks
        if b.get("run_type") == "aggregate" and b.get("aggregate_name") == "median"
    }
    if medians:
        for name, b in medians.items():
            yield {**b, "name": name}
        return
    for b in benchmarks:
        if b.get("run_type", "iteration") == "iteration":
            yield b


def gb_stddev(record, name):
    """Stddev across repetitions for one benchmark, in the entry's own units.
    None for single-repetition records, which have no aggregates."""
    for b in record["google_benchmark"].get("benchmarks", []):
        if (
            b.get("run_type") == "aggregate"
            and b.get("aggregate_name") == "stddev"
            and b.get("run_name") == name
        ):
            return b
    return None


def real_time_ns(entry):
    return entry["real_time"] * _TIME_UNIT_TO_NS.get(entry.get("time_unit", "ns"), 1.0)


def classify_gb(record):
    """Split benchmarks into ranged families, flat ns/op, and throughput."""
    ranged, flat_ns, throughput = {}, {}, {}
    for b in gb_entries(record):
        # The flash1 replay is a 2M-message stream, not a microbenchmark, and its
        # reported ns/op is instrumented. The pipeline runs it in a separate
        # invocation so these rows never reach the record; this is the belt to
        # that braces, for a hand-made unfiltered --benchmark_out=json.
        if b["name"].startswith("BM_Flash1_"):
            continue
        name, _, arg = b["name"].partition("/")
        if "items_per_second" in b:
            throughput[b["name"]] = b["items_per_second"]
        elif arg.isdigit():
            ranged.setdefault(name, []).append((int(arg), real_time_ns(b)))
        else:
            flat_ns[b["name"]] = real_time_ns(b)
    for pts in ranged.values():
        pts.sort()
    return ranged, flat_ns, throughput


def gb_metric(record, name):
    """Look up one curated metric; returns (value, is_throughput) or None."""
    for b in gb_entries(record):
        if b["name"] == name:
            if "items_per_second" in b:
                return b["items_per_second"], True
            return real_time_ns(b), False
    return None


# ------------------------------------------------------------------- summary


def fmt_si(v):
    for div, suf in ((1e9, "G"), (1e6, "M"), (1e3, "k")):
        if v >= div:
            return f"{v / div:.2f}{suf}"
    return f"{v:.0f}"


def print_summary(record, record_path):
    print()
    print(f"=== bench record: {record_path} ===")
    f1 = record["flash1"]
    if f1 is None:
        print("flash1: skipped (no harness results)")
    else:
        print(f"flash1 ({f1['reps_per_scenario']} rep(s)/scenario, median):")
        for sc in SCENARIOS:
            if sc not in f1["scenarios"]:
                continue
            s = f1["scenarios"][sc]
            base = BASELINE_MPS[sc]
            delta = (s["mps_median"] - base) / base * 100
            bad = "" if all(v == "VALID" for v in s["verdicts"]) else "  ** INVALID **"
            print(f"  {sc:<12} {s['mps_median']:8.3f} M/s   baseline {base:6.3f}   {delta:+6.1f}%{bad}")
        print(f"  worst case:  {f1['worst_case_mps']:.3f} M/s ({f1['worst_scenario']})")
        if not f1["all_valid"]:
            print("  *** ONE OR MORE RUNS INVALID — throughput numbers above are not trustworthy ***")
    print("google-benchmark headliners:")
    for name in HISTORY_THROUGHPUT:
        m = gb_metric(record, name)
        if m:
            print(f"  {name:<40} {fmt_si(m[0])} items/s")
    for name in HISTORY_LATENCY_NS:
        m = gb_metric(record, name)
        if m:
            print(f"  {name:<40} {m[0]:,.0f} ns/op")
    print_latency_summary(record)
    print()


def print_latency_summary(record):
    lat = record.get("flash1_latency")
    if not lat:
        return
    runs = [r for r in lat.get("runs", []) if r.get("instrumented") and r.get("ops")]
    if not runs:
        return
    timer = lat.get("timer") or {}
    unit = "ns" if timer.get("units") == "ns" else "ticks"
    floor = (timer.get("overhead") or {}).get("p50")
    raw_ns = {r["scenario"]: r.get("ns_per_op")
              for r in lat["runs"] if not r.get("instrumented")}

    print(f"flash1 per-op latency ({unit}, timer floor "
          f"{floor:.1f} via {timer.get('source', '?')}/{timer.get('invariant_evidence', '?')}):"
          if floor else "flash1 per-op latency:")
    if unit != "ns":
        print("  *** TSC rate unverified — these are ticks, not time ***")
    for run in sorted(runs, key=lambda r: SCENARIOS.index(r["scenario"])
                      if r["scenario"] in SCENARIOS else 99):
        sc = run["scenario"]
        raw = raw_ns.get(sc)
        tax = (f"  (+{(run['ns_per_op'] - raw) / raw * 100:.0f}% vs {raw:.0f} raw)"
               if raw and run.get("ns_per_op") else "")
        print(f"  {sc:<12} {run['ns_per_op']:6.1f} ns/op instrumented{tax}")
        for kind, _ in OP_KINDS:
            op = run["ops"].get(kind)
            if not op or not op.get("count"):
                continue
            print(f"    {kind:<9} n={op['count']:>9,}  p50={op['p50']:>8.1f}  "
                  f"p99={op['p99']:>9.1f}  p99.9={op['p999']:>9.1f}  max={op['max']:>10.1f}")
        counters = run.get("counters", {})
        if counters.get("cancel_rejects", 0) == 0:
            print("    *** zero rejected cancels — the stream did not really replay ***")
        if counters.get("timer_anomalies", 0):
            print(f"    *** {counters['timer_anomalies']} backwards TSC deltas — "
                  "thread migrated across unsynchronised cores ***")


# --------------------------------------------------------------------- plots


def apply_style():
    import matplotlib as mpl

    mpl.rcParams.update({
        "figure.facecolor": SURFACE,
        "axes.facecolor": SURFACE,
        "savefig.facecolor": SURFACE,
        "text.color": INK,
        "axes.labelcolor": INK2,
        "axes.titlecolor": INK2,
        "axes.titlesize": 10,
        "axes.labelsize": 9,
        "xtick.color": MUTED,
        "ytick.color": MUTED,
        "xtick.labelsize": 8,
        "ytick.labelsize": 8,
        "axes.edgecolor": AXIS,
        "axes.linewidth": 0.8,
        "axes.spines.top": False,
        "axes.spines.right": False,
        "axes.grid": True,
        "grid.color": GRID,
        "grid.linewidth": 0.8,
        "grid.linestyle": "-",
        "legend.frameon": False,
        "legend.fontsize": 8,
        "legend.labelcolor": INK2,
        "font.family": "sans-serif",
    })


def savefig(fig, out_dir, stem, svg):
    out_dir.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_dir / f"{stem}.png", dpi=160, bbox_inches="tight")
    if svg:
        fig.savefig(out_dir / f"{stem}.svg", bbox_inches="tight")
    import matplotlib.pyplot as plt

    plt.close(fig)


def run_label(record):
    return record["git_sha_short"] + ("*" if record["git_dirty"] else "")


def plot_flash1_latest(record, plots_dir, svg):
    import matplotlib.pyplot as plt

    f1 = record["flash1"]
    scenarios = [s for s in SCENARIOS if s in f1["scenarios"]]
    xs = range(len(scenarios))
    meds = [f1["scenarios"][s]["mps_median"] for s in scenarios]
    valid = [all(v == "VALID" for v in f1["scenarios"][s]["verdicts"]) for s in scenarios]

    fig, ax = plt.subplots(figsize=(7.2, 4.2))
    ax.set_yscale("log")
    ax.scatter(xs, [BASELINE_MPS[s] for s in scenarios], marker="_", s=260, linewidths=2,
               color=MUTED, label="baseline (Jul 2026)", zorder=2)
    # Min-max across reps rather than a stddev: with only a handful of reps the
    # observed range is the honest statement of what was actually seen, and it
    # makes a scenario whose reps disagree impossible to read as a clean point.
    reps = [f1["scenarios"][s]["mps"] for s in scenarios]
    if any(len(r) > 1 for r in reps):
        ax.errorbar(xs, meds,
                    yerr=[[m - min(r) for m, r in zip(meds, reps)],
                          [max(r) - m for m, r in zip(meds, reps)]],
                    fmt="none", ecolor=SERIES[0], elinewidth=1.4, capsize=4,
                    alpha=0.65, zorder=3)
    ax.scatter(xs, meds, s=70, color=SERIES[0],
               label=f"this run (median of {f1['reps_per_scenario']})", zorder=4)
    for x, s, m, ok in zip(xs, scenarios, meds, valid):
        ax.annotate(f"{m:.3f}", (x, m), textcoords="offset points", xytext=(0, 9),
                    ha="center", fontsize=8, color=INK2)
        if not ok:
            ax.scatter([x], [m], s=170, facecolors="none", edgecolors=CRITICAL,
                       linewidths=2, zorder=4)
            ax.annotate("✗ INVALID", (x, m), textcoords="offset points", xytext=(0, -16),
                        ha="center", fontsize=8, color=CRITICAL, fontweight="bold")
    worst_x = scenarios.index(f1["worst_scenario"])
    ax.annotate(f"worst case\n{f1['worst_case_mps']:.3f} M/s", (worst_x, meds[worst_x]),
                textcoords="offset points", xytext=(0, -30), ha="center",
                fontsize=8, color=INK)
    ax.set_xticks(list(xs), scenarios)
    ax.set_ylabel("throughput (M msgs/s, log)")
    ax.set_title(f"flash1 per-scenario throughput — {run_label(record)}  ({record['timestamp_utc']})")
    ax.legend(loc="upper right")
    ax.margins(y=0.25)
    savefig(fig, plots_dir, "flash1_latest", svg)


def plot_flash1_history(history, plots_dir, svg):
    import matplotlib.pyplot as plt

    runs = [r for r in history if r["flash1"]]
    if not runs:
        return False
    fig, ax = plt.subplots(figsize=(7.8, 4.4))
    ax.set_yscale("log")
    xs = range(len(runs))
    end_labels = []
    for i, sc in enumerate(SCENARIOS):
        ys = [r["flash1"]["scenarios"].get(sc, {}).get("mps_median", math.nan) for r in runs]
        if all(math.isnan(y) for y in ys):
            continue
        # Rep spread as a band, same reasoning as the GB trend: a bare line
        # invites reading run-to-run drift as a change in the engine.
        reps = [r["flash1"]["scenarios"].get(sc, {}).get("mps", []) for r in runs]
        los = [min(rp) if len(rp) > 1 else y for rp, y in zip(reps, ys)]
        his = [max(rp) if len(rp) > 1 else y for rp, y in zip(reps, ys)]
        ax.fill_between(list(xs), los, his, color=SERIES[i], alpha=0.15, linewidth=0)
        ax.plot(xs, ys, color=SERIES[i], linewidth=1.8, marker="o", markersize=5, label=sc)
        last = max((j for j, y in enumerate(ys) if not math.isnan(y)), default=None)
        if last is not None:
            end_labels.append((ys[last], last, f"{sc}  {ys[last]:.3f}", SERIES[i]))

    # Scenarios converge once the engine stops collapsing on the volatile ones,
    # and four end-labels at the same height are unreadable. Stack them in value
    # order with a fixed point offset rather than pinning each to its own line.
    end_labels.sort(key=lambda t: t[0])
    span = 11 * (len(end_labels) - 1) / 2
    for k, (y, x, text, colour) in enumerate(end_labels):
        ax.annotate(text, (x, y), textcoords="offset points",
                    xytext=(9, k * 11 - span), va="center", fontsize=8, color=colour)
    ax.set_xticks(list(xs), [run_label(r) for r in runs], rotation=45, ha="right")
    ax.set_ylabel("median throughput (M msgs/s, log)")
    ax.set_title("flash1 throughput over runs  (* = dirty tree)")
    ax.legend(loc="lower left", ncols=2)
    ax.margins(x=0.15)
    savefig(fig, plots_dir, "flash1_history", svg)
    return True


def plot_gb_latest(record, plots_dir, svg):
    import matplotlib.pyplot as plt

    ranged, flat_ns, throughput = classify_gb(record)
    n_panels = len(ranged) + (1 if flat_ns else 0) + (1 if throughput else 0)
    if n_panels == 0:
        return False
    ncols = 3
    nrows = math.ceil(n_panels / ncols)
    fig, axes = plt.subplots(nrows, ncols, figsize=(3.6 * ncols, 3.0 * nrows))
    axes = [ax for row in (axes if nrows > 1 else [axes]) for ax in row]

    idx = 0
    for family, pts in sorted(ranged.items()):
        ax = axes[idx]
        idx += 1
        args_, ns = zip(*pts)
        ax.plot(args_, ns, color=SERIES[0], linewidth=1.8, marker="o", markersize=4)
        ax.set_xscale("log", base=2)
        ax.set_yscale("log")
        ax.set_title(family)
        ax.set_xlabel("arg")
        ax.set_ylabel("ns/op")

    def bar_panel(ax, items, unit, fmt):
        from matplotlib.ticker import FuncFormatter

        names = list(items)
        vals = [items[n] for n in names]
        ys = range(len(names))
        ax.xaxis.set_major_formatter(FuncFormatter(lambda v, _: fmt_si(v) if v else "0"))
        ax.barh(ys, vals, height=0.55, color=SERIES[0])
        ax.set_yticks(list(ys), [n.removeprefix("BM_") for n in names])
        ax.invert_yaxis()
        ax.set_xlabel(unit)
        ax.tick_params(axis="y", labelsize=7)
        for y, v in zip(ys, vals):
            ax.annotate(fmt(v), (v, y), textcoords="offset points", xytext=(4, 0),
                        va="center", fontsize=7, color=INK2)
        ax.margins(x=0.18)
        ax.grid(axis="y", visible=False)

    if flat_ns:
        bar_panel(axes[idx], flat_ns, "ns/op", lambda v: f"{v:,.0f}")
        axes[idx].set_title("flat benchmarks")
        idx += 1
    if throughput:
        bar_panel(axes[idx], throughput, "items/s", lambda v: fmt_si(v))
        axes[idx].set_title("throughput benchmarks")
        idx += 1
    for ax in axes[idx:]:
        ax.set_visible(False)
    fig.suptitle(f"Google Benchmark — {run_label(record)}  ({record['timestamp_utc']})",
                 fontsize=11, color=INK)
    fig.tight_layout(rect=(0, 0, 1, 0.97))
    savefig(fig, plots_dir, "gb_latest", svg)
    return True


def plot_gb_history(history, plots_dir, svg):
    import matplotlib.pyplot as plt

    runs = [r for r in history if r.get("google_benchmark")]
    if not runs:
        return False
    fig, (ax_tp, ax_ns) = plt.subplots(2, 1, figsize=(7.8, 6.6), sharex=True)
    xs = range(len(runs))

    def series(ax, names, pick_throughput, ylabel):
        for i, name in enumerate(names):
            ys, los, his = [], [], []
            for r in runs:
                m = gb_metric(r, name)
                y = m[0] if m and m[1] == pick_throughput else math.nan
                ys.append(y)
                # Spread across repetitions. Without it a few-percent step
                # between two runs is unreadable — which is exactly how machine
                # drift once got mistaken for a regression.
                sd = gb_stddev(r, name)
                d = None
                if sd is not None and not math.isnan(y):
                    d = sd.get("items_per_second") if pick_throughput else real_time_ns(sd)
                los.append(y if d is None else y - d)
                his.append(y if d is None else y + d)
            if all(math.isnan(y) for y in ys):
                continue
            colour = SERIES[i % len(SERIES)]
            ax.fill_between(list(xs), los, his, color=colour, alpha=0.18, linewidth=0)
            ax.plot(xs, ys, color=colour, linewidth=1.8, marker="o", markersize=5,
                    label=name.removeprefix("BM_"))
        ax.set_yscale("log")
        ax.set_ylabel(ylabel)
        ax.legend(loc="best")

    series(ax_tp, HISTORY_THROUGHPUT, True, "items/s (log)")
    series(ax_ns, HISTORY_LATENCY_NS, False, "ns/op (log)")
    ax_ns.set_xticks(list(xs), [run_label(r) for r in runs], rotation=45, ha="right")
    ax_tp.set_title("Google Benchmark key metrics over runs  (* = dirty tree)")
    fig.tight_layout()
    savefig(fig, plots_dir, "gb_history", svg)
    return True


# The four message kinds the flash1 replay separates. Colour is the op-kind
# channel here, which is why the percentile marks below are drawn in ink with
# different linestyles rather than in the QUANTILES colours plot_net_bench.py
# uses — there, quantiles are the series; here, they are annotation.
OP_KINDS = [("new", SERIES[0]), ("new_ioc", SERIES[1]),
            ("cancel", SERIES[2]), ("modify", SERIES[3])]
OP_LABELS = {"new": "new", "new_ioc": "new (IOC)", "cancel": "cancel",
             "modify": "modify"}
QUANTILE_MARKS = [(0.50, "-", "p50"), (0.99, "--", "p99"), (0.999, ":", "p99.9")]


def merged_quantiles(ops, quantiles):
    """Quantiles over all op kinds pooled, from the per-kind bucket arrays.

    Exact with respect to the bucketed data: each bucket contributes its count
    at its own upper edge, which is the same upper-edge convention the C++ side
    reports per kind.
    """
    pairs = []
    for op in ops.values():
        edges, counts = op.get("edges") or [], op.get("counts") or []
        pairs += [(edges[i + 1], c) for i, c in enumerate(counts) if c]
    if not pairs:
        return {}
    pairs.sort()
    total = sum(c for _, c in pairs)
    out = {}
    for q in quantiles:
        target, seen = q * total, 0
        out[q] = pairs[-1][0]
        for edge, count in pairs:
            seen += count
            if seen > target:
                out[q] = edge
                break
    return out


def rebin_log(edges, counts, bins_per_decade=26, resolution=0.0):
    """Aggregate the sidecar's ~3%-resolution buckets onto a coarser display grid.

    Display only — the record keeps full resolution. Two things force this. The
    stored buckets are finer than the timer's own quantum near the floor, so
    plotting them faithfully draws a picket fence of alternating occupied and
    empty buckets that reads as noise; and the TSC does not advance one count at
    a time (see TimerCalibration::resolution_ticks), so no bin narrower than
    that quantum can ever be occupied.

    Hence a grid that is geometric in the body and at least one quantum wide at
    the low end — which is where the two constraints disagree.
    """
    lo = next((e for e in edges if e > 0), None)
    hi = edges[-1]
    if lo is None or hi <= lo:
        return edges, counts

    ratio = 10 ** (1.0 / bins_per_decade)
    new_edges, edge = [lo], lo
    while edge < hi:
        edge = max(edge * ratio, edge + resolution)
        new_edges.append(edge)
    new_counts = [0] * (len(new_edges) - 1)
    index = 0
    for i, count in enumerate(counts):
        if not count:
            continue
        while index + 1 < len(new_counts) and edges[i] >= new_edges[index + 1]:
            index += 1
        new_counts[index] += count
    return new_edges, new_counts


def plot_flash1_latency(record, plots_dir, svg):
    """Per-operation latency distribution: x = time of one operation, y = how
    often. Log on both axes — the mode is ~1e6 samples and the tail is ~1e0, so
    on a linear y the tail, which is the entire reason for the plot, is a flat
    line lying on the axis."""
    import matplotlib.pyplot as plt
    from matplotlib.lines import Line2D
    from matplotlib.patches import Patch

    lat = record.get("flash1_latency")
    if not lat:
        return False
    runs = [r for r in lat.get("runs", []) if r.get("instrumented") and r.get("ops")]
    if not runs:
        return False
    raw_ns = {r["scenario"]: r.get("ns_per_op")
              for r in lat["runs"] if not r.get("instrumented")}
    by_scenario = {r["scenario"]: r for r in runs}
    scenarios = [s for s in SCENARIOS if s in by_scenario]
    scenarios += [s for s in by_scenario if s not in SCENARIOS]

    timer = lat.get("timer") or {}
    in_ns = timer.get("units") == "ns"
    unit = "ns" if in_ns else "TSC ticks"

    ncols = 2
    nrows = math.ceil(len(scenarios) / ncols)
    fig, axes = plt.subplots(nrows, ncols, figsize=(11.5, 3.3 * nrows + 1.0))
    axes = axes.flatten() if len(scenarios) > 1 else [axes]

    for ax, scenario in zip(axes, scenarios):
        run = by_scenario[scenario]
        ops = run["ops"]
        for kind, colour in OP_KINDS:
            op = ops.get(kind)
            if not op or not op.get("counts"):
                continue
            edges, counts = rebin_log(op["edges"], op["counts"],
                                      resolution=timer.get("resolution") or 0.0)
            ax.stairs(counts, edges, color=colour, fill=True, alpha=0.12, linewidth=0)
            ax.stairs(counts, edges, color=colour, linewidth=1.3)

        marks = merged_quantiles(ops, [q for q, _, _ in QUANTILE_MARKS])
        for q, style, _ in QUANTILE_MARKS:
            if q in marks:
                ax.axvline(marks[q], color=INK2, linestyle=style, linewidth=1.0, alpha=0.8)
        # Everything left of this line is instrument, not engine.
        floor = (timer.get("overhead") or {}).get("p50")
        if floor:
            ax.axvline(floor, color=CRITICAL, linestyle="--", linewidth=1.1, alpha=0.9)

        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_xlabel(f"latency per operation ({unit}, log)")
        ax.set_ylabel("count (log)")

        title = scenario
        instrumented = run.get("ns_per_op")
        raw = raw_ns.get(scenario)
        if instrumented and raw:
            title += (f" — {instrumented:.0f} ns/op instrumented, {raw:.0f} raw "
                      f"(+{(instrumented - raw) / raw * 100:.0f}%)")
        ax.set_title(title)

    for ax in axes[len(scenarios):]:
        ax.set_visible(False)

    handles = [Patch(facecolor=c, alpha=0.35, edgecolor=c, label=OP_LABELS[k])
               for k, c in OP_KINDS]
    handles += [Line2D([], [], color=INK2, linestyle=s, label=f"{n} (all kinds)")
                for _, s, n in QUANTILE_MARKS]
    if (timer.get("overhead") or {}).get("p50"):
        handles.append(Line2D([], [], color=CRITICAL, linestyle="--",
                              label="timer floor"))
    fig.legend(handles=handles, loc="lower center", ncol=min(4, len(handles)),
               bbox_to_anchor=(0.5, -0.02))

    suptitle = (f"flash1 per-operation latency — {run_label(record)}, "
                f"{lat.get('workload', {}).get('count', '?')} orders/scenario")
    fig.suptitle(suptitle, fontsize=11, color=INK)
    if not in_ns:
        fig.text(0.5, 0.955, "TSC rate could not be verified — x axis is ticks, not time",
                 ha="center", fontsize=9, color=CRITICAL)
    fig.tight_layout(rect=(0, 0.02, 1, 0.96))
    savefig(fig, plots_dir, "flash1_latency", svg)
    return True


# ---------------------------------------------------------------------- main


def main(argv=None):
    args = parse_args(argv)
    out_dir = args.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)
    plots_dir = out_dir / "plots"

    record_path = None
    if args.gb_json:
        record = assemble_record(args)
        record_path = write_record(record, out_dir, args.timestamp)

    history = load_history(out_dir)
    if not history:
        print(f"error: no records in {out_dir} — run scripts/bench_pipeline.sh first", file=sys.stderr)
        return 1
    latest = history[-1]
    # Summarise the record just written, not history[-1]. They differ whenever a
    # backdated --timestamp is used (bench_backfill.sh always does), and printing
    # the newest-by-timestamp record there reports some *other* commit's numbers
    # under the heading of the run that just finished.
    written_record = record if args.gb_json else latest
    print_summary(written_record, record_path or "(latest existing record)")

    try:
        apply_style()
    except ImportError:
        print("error: matplotlib unavailable — run via `uv run scripts/plot_bench.py` "
              "(record was still written)", file=sys.stderr)
        return 3

    written = []
    if latest["flash1"]:
        plot_flash1_latest(latest, plots_dir, args.svg)
        written.append("flash1_latest.png")
    if plot_flash1_history(history, plots_dir, args.svg):
        written.append("flash1_history.png")
    if plot_gb_latest(latest, plots_dir, args.svg):
        written.append("gb_latest.png")
    if plot_gb_history(history, plots_dir, args.svg):
        written.append("gb_history.png")
    # Latest-only: a distribution does not reduce to one number, so there is no
    # trend counterpart. Note that adding one of these to HISTORY_LATENCY_NS
    # would not work — gb_metric reads record["google_benchmark"], and these
    # numbers live under "flash1_latency".
    if plot_flash1_latency(latest, plots_dir, args.svg):
        written.append("flash1_latency.png")
    print(f"plots: {plots_dir}/{{{','.join(written)}}}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
