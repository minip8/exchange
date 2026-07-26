#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = ["matplotlib"]
# ///
"""Assemble a network benchmark run record and render plots.

Invoked by scripts/net_bench_pipeline.sh with fresh net_workload_bench output
(and optionally net_loopback_bench output); also runnable standalone without
--net-json to re-plot from the existing history in bench/results/net/.

Kept separate from plot_bench.py on purpose. That one plots one number per
scenario; this one plots a latency distribution across a rate sweep, and
flattening it to a median to fit the other shape would throw away the only
interesting part — where the knee is.

One self-contained JSON record per pipeline run:
  bench/results/net/run_<UTCts>_<shortsha>[-dirty].json
Plots (regenerated from full history every run):
  bench/results/net/plots/{net_latency_vs_rate,net_throughput,
                           net_scaling,net_history}.png
"""

import argparse
import json
import math
import os
import sys
from datetime import datetime, timezone
from pathlib import Path

NET_SCHEMA_VERSION = 1

# Canonical scenario order; also the fixed color-slot assignment. Same order as
# plot_bench.py, so a scenario is the same color in both sets of plots.
SCENARIOS = ["static", "normal", "swing-25", "swing-40", "flash-crash"]

# Light-mode reference palette (validated fixed order — do not re-order).
SERIES = ["#2a78d6", "#008300", "#e87ba4", "#eda100", "#1baf7a"]
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK2 = "#52514e"
MUTED = "#898781"
GRID = "#e1e0d9"
AXIS = "#c3c2b7"
CRITICAL = "#d03b3b"

# The three quantiles worth drawing. More lines than this and the panel stops
# being readable; fewer and the tail — the reason percentiles are here at all —
# goes missing.
QUANTILES = [("p50", SERIES[0]), ("p99", SERIES[3]), ("p999", CRITICAL)]


def parse_args(argv):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--net-json", type=Path, help="fresh net_workload_bench JSON; omit to only re-plot history")
    p.add_argument("--loopback-json", type=Path, default=None)
    p.add_argument("--harness-commit", default=None)
    p.add_argument("--sha", default=None)
    p.add_argument("--dirty", choices=["true", "false"], default="false")
    p.add_argument("--timestamp", default=None, help="compact UTC, e.g. 20260718T051203Z")
    p.add_argument("--mode", choices=["quick", "full"], default="quick")
    p.add_argument("--out-dir", type=Path,
                   default=Path(__file__).resolve().parent.parent / "bench" / "results" / "net")
    p.add_argument("--svg", action="store_true", help="also write SVG next to each PNG")
    args = p.parse_args(argv)
    if args.net_json and not (args.sha and args.timestamp):
        p.error("--net-json requires --sha and --timestamp")
    return args


# ---------------------------------------------------------------- record I/O


def assemble_record(args):
    net = json.loads(args.net_json.read_text())

    loopback = None
    if args.loopback_json:
        try:
            loopback = json.loads(args.loopback_json.read_text())
        except (OSError, json.JSONDecodeError) as e:
            print(f"warning: skipping unreadable loopback result: {e}", file=sys.stderr)

    ts = datetime.strptime(args.timestamp, "%Y%m%dT%H%M%SZ").replace(tzinfo=timezone.utc)
    return {
        "net_schema": NET_SCHEMA_VERSION,
        "timestamp_utc": ts.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "git_sha": args.sha,
        "git_sha_short": args.sha[:7],
        "git_dirty": args.dirty == "true",
        "mode": args.mode,
        "harness_commit": args.harness_commit,
        "workload": net.get("workload", {}),
        "config": net.get("config", {}),
        "scenarios": net.get("scenarios", {}),
        "loopback": loopback,
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
        if rec.get("net_schema") == NET_SCHEMA_VERSION:
            records.append(rec)
    records.sort(key=lambda r: r["timestamp_utc"])
    return records


# ------------------------------------------------------------------ accessors


def scenarios_in(record):
    """Present scenarios, in canonical order, with any unknown names appended."""
    present = record.get("scenarios", {})
    known = [s for s in SCENARIOS if s in present]
    return known + [s for s in present if s not in SCENARIOS]


def rates_of(record, scenario):
    return record.get("scenarios", {}).get(scenario, {}).get("tcp", {}).get("rates", [])


def paced_rates(record, scenario):
    """The rate sweep, saturation excluded — it has no offered rate to plot."""
    return sorted((r for r in rates_of(record, scenario) if r["offered"] > 0),
                  key=lambda r: r["offered"])


def saturation_of(record, scenario):
    for rate in rates_of(record, scenario):
        if rate["offered"] == 0:
            return rate
    return None


def gateway_of(record, scenario):
    return record.get("scenarios", {}).get(scenario, {}).get("gateway")


def scaling_of(record, scenario):
    return sorted(record.get("scenarios", {}).get(scenario, {}).get("scaling", []),
                  key=lambda r: r["clients"])


def problems(rate):
    """Anything that makes a row's numbers untrustworthy rather than merely bad."""
    flags = []
    if rate.get("disconnected"):
        flags.append("disconnected")
    if rate.get("desync"):
        flags.append(f"{rate['desync']} desync")
    if rate.get("error"):
        flags.append(rate["error"])
    return flags


# ------------------------------------------------------------------- summary


def fmt_si(v):
    for div, suf in ((1e9, "G"), (1e6, "M"), (1e3, "k")):
        if v >= div:
            return f"{v / div:.2f}{suf}"
    return f"{v:.0f}"


def print_summary(record, record_path):
    print()
    print(f"=== net bench record: {record_path} ===")
    workload = record.get("workload", {})
    config = record.get("config", {})
    print(f"workload: seed {workload.get('seed')}, {workload.get('count')} orders requested")
    print(f"server:   io_threads={config.get('io_threads')} egress={config.get('egress')} "
          f"spin_us={config.get('spin_us')} window={config.get('window')}")
    print(f"{'scenario':<13} {'gateway':>10} {'tcp sat':>10} {'p50':>9} {'p99':>9} {'p99.9':>10}")
    bad = []
    for scenario in scenarios_in(record):
        gateway = gateway_of(record, scenario)
        sat = saturation_of(record, scenario)
        gw = f"{gateway['msgs_per_s'] / 1e6:.3f}M" if gateway else "-"
        if sat:
            print(f"  {scenario:<11} {gw:>10} {sat['achieved'] / 1e6:>9.3f}M "
                  f"{sat['ns']['p50'] / 1000:>8.1f}us {sat['ns']['p99'] / 1000:>8.1f}us "
                  f"{sat['ns']['p999'] / 1000:>9.1f}us")
        else:
            print(f"  {scenario:<11} {gw:>10} {'-':>10}")
        for rate in rates_of(record, scenario) + scaling_of(record, scenario):
            for flag in problems(rate):
                bad.append(f"{scenario} @ {rate['offered'] or 'saturation'}: {flag}")
    if bad:
        print()
        print("  *** these rows are not trustworthy ***")
        for line in bad:
            print(f"    {line}")
    print()


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


def plot_latency_vs_rate(record, plots_dir, svg):
    """The core plot: latency against offered load, one panel per scenario.

    The saturation run has no offered rate, so it appears as a vertical marker
    at the throughput it actually reached — which is precisely the wall the
    paced curves are bending towards.
    """
    import matplotlib.pyplot as plt

    scenarios = [s for s in scenarios_in(record) if paced_rates(record, s)]
    if not scenarios:
        return False
    ncols = min(3, len(scenarios))
    nrows = math.ceil(len(scenarios) / ncols)
    fig, axes = plt.subplots(nrows, ncols, figsize=(4.0 * ncols, 3.2 * nrows),
                             squeeze=False)
    flat = [ax for row in axes for ax in row]

    for ax, scenario in zip(flat, scenarios):
        rates = paced_rates(record, scenario)
        xs = [r["offered"] for r in rates]
        for key, color in QUANTILES:
            ys = [r["ns"][key] / 1000.0 for r in rates]
            ax.plot(xs, ys, color=color, linewidth=1.8, marker="o", markersize=4,
                    label=key.replace("p999", "p99.9"))
        sat = saturation_of(record, scenario)
        if sat and sat["achieved"] > 0:
            ax.axvline(sat["achieved"], color=MUTED, linestyle="--", linewidth=1.2)
            ax.annotate(f"saturation\n{sat['achieved'] / 1e6:.2f}M/s",
                        (sat["achieved"], ax.get_ylim()[1]), textcoords="offset points",
                        xytext=(-4, -12), ha="right", va="top", fontsize=7, color=MUTED)
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_title(scenario)
        ax.set_xlabel("offered rate (msgs/s, log)")
        ax.set_ylabel("ack latency (us, log)")
    flat[0].legend(loc="upper left")
    for ax in flat[len(scenarios):]:
        ax.set_visible(False)
    fig.suptitle(f"Order-ack latency vs offered load — {run_label(record)}  "
                 f"({record['timestamp_utc']})", fontsize=11, color=INK)
    fig.tight_layout(rect=(0, 0, 1, 0.96))
    savefig(fig, plots_dir, "net_latency_vs_rate", svg)
    return True


def plot_throughput(record, plots_dir, svg):
    """Gateway floor against the TCP ceiling, per scenario.

    The gap between the two bars is the transport, and it is the number this
    whole pipeline exists to put a value on.
    """
    import matplotlib.pyplot as plt

    scenarios = scenarios_in(record)
    gateways = [gateway_of(record, s) for s in scenarios]
    sats = [saturation_of(record, s) for s in scenarios]
    if not any(gateways) and not any(sats):
        return False

    fig, ax = plt.subplots(figsize=(7.6, 4.2))
    xs = range(len(scenarios))
    width = 0.38
    gw = [(g["msgs_per_s"] / 1e6) if g else math.nan for g in gateways]
    tp = [(s["achieved"] / 1e6) if s else math.nan for s in sats]
    ax.bar([x - width / 2 for x in xs], gw, width, color=SERIES[0],
           label="gateway (no sockets)")
    ax.bar([x + width / 2 for x in xs], tp, width, color=SERIES[4],
           label="tcp saturation")
    # Linear, not log. A bar reads as a length from zero, and a log axis has no
    # zero — on one, a bar 37% of its neighbour's value draws as a sliver.
    ax.set_ylim(bottom=0)
    for x, v in zip(xs, gw):
        if not math.isnan(v):
            ax.annotate(f"{v:.2f}", (x - width / 2, v), textcoords="offset points",
                        xytext=(0, 3), ha="center", fontsize=7, color=INK2)
    for x, v in zip(xs, tp):
        if not math.isnan(v):
            ax.annotate(f"{v:.2f}", (x + width / 2, v), textcoords="offset points",
                        xytext=(0, 3), ha="center", fontsize=7, color=INK2)
    ax.set_xticks(list(xs), scenarios)
    ax.set_ylabel("throughput (M msgs/s)")
    ax.set_title(f"flash1 workload throughput — {run_label(record)}  "
                 f"({record['timestamp_utc']})")
    ax.legend(loc="upper right")
    ax.grid(axis="x", visible=False)
    savefig(fig, plots_dir, "net_throughput", svg)
    return True


def plot_scaling(record, plots_dir, svg):
    """What a second, fourth, eighth connection buys — and what it costs."""
    import matplotlib.pyplot as plt

    scenarios = [s for s in scenarios_in(record) if scaling_of(record, s)]
    if not scenarios:
        return False
    fig, (ax_tp, ax_ns) = plt.subplots(1, 2, figsize=(9.2, 3.8))
    for i, scenario in enumerate(scenarios):
        rows = scaling_of(record, scenario)
        xs = [r["clients"] for r in rows]
        color = SERIES[SCENARIOS.index(scenario) % len(SERIES)] \
            if scenario in SCENARIOS else SERIES[i % len(SERIES)]
        ax_tp.plot(xs, [r["achieved"] / 1e6 for r in rows], color=color,
                   linewidth=1.8, marker="o", markersize=4, label=scenario)
        ax_ns.plot(xs, [r["ns"]["p99"] / 1000.0 for r in rows], color=color,
                   linewidth=1.8, marker="o", markersize=4, label=scenario)
    for ax, ylabel in ((ax_tp, "saturation throughput (M msgs/s)"),
                       (ax_ns, "p99 ack latency (us)")):
        ax.set_xscale("log", base=2)
        ax.set_xlabel("connections")
        ax.set_ylabel(ylabel)
    # From zero, so "scales sublinearly" and "barely moves" stay visually
    # distinct rather than both filling the panel.
    ax_tp.set_ylim(bottom=0)
    ax_ns.set_ylim(bottom=0)
    ax_tp.legend(loc="best", ncols=2)
    fig.suptitle(f"Connection scaling — {run_label(record)}", fontsize=11, color=INK)
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    savefig(fig, plots_dir, "net_scaling", svg)
    return True


def plot_history(history, plots_dir, svg):
    """Saturation throughput and p99 per scenario across runs."""
    import matplotlib.pyplot as plt

    runs = [r for r in history if any(saturation_of(r, s) for s in scenarios_in(r))]
    if not runs:
        return False
    fig, (ax_tp, ax_ns) = plt.subplots(2, 1, figsize=(7.8, 6.6), sharex=True)
    xs = range(len(runs))
    for i, scenario in enumerate(SCENARIOS):
        tps, p99s = [], []
        for run in runs:
            sat = saturation_of(run, scenario)
            tps.append(sat["achieved"] / 1e6 if sat else math.nan)
            p99s.append(sat["ns"]["p99"] / 1000.0 if sat else math.nan)
        if all(math.isnan(v) for v in tps):
            continue
        ax_tp.plot(xs, tps, color=SERIES[i], linewidth=1.8, marker="o",
                   markersize=5, label=scenario)
        ax_ns.plot(xs, p99s, color=SERIES[i], linewidth=1.8, marker="o",
                   markersize=5, label=scenario)
    ax_tp.set_ylabel("saturation (M msgs/s)")
    ax_ns.set_ylabel("p99 ack latency (us)")
    ax_ns.set_xticks(list(xs), [run_label(r) for r in runs], rotation=45, ha="right")
    ax_tp.set_title("Network benchmark over runs  (* = dirty tree)")
    ax_tp.legend(loc="best", ncols=2)
    fig.tight_layout()
    savefig(fig, plots_dir, "net_history", svg)
    return True


# ---------------------------------------------------------------------- main


def main(argv=None):
    args = parse_args(argv)
    out_dir = args.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)
    plots_dir = out_dir / "plots"

    record_path = None
    if args.net_json:
        record = assemble_record(args)
        record_path = write_record(record, out_dir, args.timestamp)

    history = load_history(out_dir)
    if not history:
        print(f"error: no records in {out_dir} — run scripts/net_bench_pipeline.sh first",
              file=sys.stderr)
        return 1
    latest = history[-1]
    print_summary(latest, record_path or "(latest existing record)")

    try:
        apply_style()
    except ImportError:
        print("error: matplotlib unavailable — run via `uv run scripts/plot_net_bench.py` "
              "(record was still written)", file=sys.stderr)
        return 3

    written = []
    if plot_latency_vs_rate(latest, plots_dir, args.svg):
        written.append("net_latency_vs_rate.png")
    if plot_throughput(latest, plots_dir, args.svg):
        written.append("net_throughput.png")
    if plot_scaling(latest, plots_dir, args.svg):
        written.append("net_scaling.png")
    if plot_history(history, plots_dir, args.svg):
        written.append("net_history.png")
    print(f"plots: {plots_dir}/{{{','.join(written)}}}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
