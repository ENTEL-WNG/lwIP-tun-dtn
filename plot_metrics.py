#!/usr/bin/env python3
"""
plot_metrics.py — Generate publication-quality line graphs from metrics.jsonl.

Reads <captures>/metrics.jsonl written by dtn_exporter during the experiment.
No running containers or Prometheus instance needed.

Usage:
  python3 plot_metrics.py --captures networks/<plan>/captures/<N> [options]

Options:
  --captures DIR    Capture directory containing metrics.jsonl (required)
  --out DIR         Output directory for plots (default: <captures>/plots)
  --fmt FORMAT      pdf, svg, or png (default: svg)

Dependencies:
  pip install matplotlib
"""

import argparse
import json
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt
import matplotlib.ticker as mticker

# ---------------------------------------------------------------------------
# Matplotlib style for papers
# ---------------------------------------------------------------------------

plt.rcParams.update({
    "font.family":       "serif",
    "font.size":         10,
    "axes.titlesize":    11,
    "axes.labelsize":    10,
    "xtick.labelsize":   9,
    "ytick.labelsize":   9,
    "legend.fontsize":   9,
    "lines.linewidth":   1.5,
    "axes.grid":         True,
    "grid.alpha":        0.3,
    "figure.dpi":        150,
    "savefig.bbox":      "tight",
    "savefig.dpi":       300,
})

COLORS = plt.rcParams["axes.prop_cycle"].by_key()["color"]


# ---------------------------------------------------------------------------
# Data loading
# ---------------------------------------------------------------------------

def load(jsonl_path: Path) -> dict[str, list[dict]]:
    """
    Read metrics.jsonl and return {node: [record, ...]} sorted by timestamp.
    Each record has keys: ts, cpu_pct, mem_rss_bytes, mem_total_bytes,
    net_rx_bytes, net_tx_bytes, db_stored_packets, db_avg_delivery_sec.
    Missing keys are None.
    """
    by_node: dict[str, list] = defaultdict(list)
    with open(jsonl_path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                r = json.loads(line)
                by_node[r["node"]].append(r)
            except (json.JSONDecodeError, KeyError):
                continue
    for records in by_node.values():
        records.sort(key=lambda r: r["ts"])
    return dict(by_node)


def _rel_times(records: list[dict], t0: float) -> list[float]:
    return [r["ts"] - t0 for r in records]


def _values(records: list[dict], key: str) -> list:
    return [r.get(key) for r in records]


def _rate(records: list[dict], key: str) -> tuple[list[float], list[float]]:
    """Differentiate a cumulative counter → bytes/sec."""
    times, rates = [], []
    for i in range(1, len(records)):
        dt = records[i]["ts"] - records[i - 1]["ts"]
        v0, v1 = records[i - 1].get(key), records[i].get(key)
        if dt > 0 and v0 is not None and v1 is not None:
            times.append(records[i]["ts"])
            rates.append(max(0.0, (v1 - v0) / dt))
    return times, rates


def _global_t0(by_node: dict) -> float:
    return min(r["ts"] for recs in by_node.values() for r in recs)


# ---------------------------------------------------------------------------
# Individual plots
# ---------------------------------------------------------------------------

def _save(fig: plt.Figure, path: Path, fmt: str) -> None:
    out = path.with_suffix(f".{fmt}")
    fig.savefig(out)
    plt.close(fig)
    print(f"  wrote {out}")


def plot_cpu(by_node: dict, t0: float, out: Path, fmt: str) -> None:
    nodes = [n for n, recs in by_node.items()
             if any(r.get("cpu_pct") is not None for r in recs)]
    if not nodes:
        print("  [skip] cpu: no data (DB_ONLY mode?)")
        return
    fig, ax = plt.subplots(figsize=(6, 3))
    for i, node in enumerate(sorted(nodes)):
        recs = by_node[node]
        ts   = _rel_times(recs, t0)
        vals = _values(recs, "cpu_pct")
        pairs = [(t, v) for t, v in zip(ts, vals) if v is not None]
        if pairs:
            ax.plot(*zip(*pairs), label=node, color=COLORS[i % len(COLORS)])
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("CPU (%)")
    ax.set_title("CPU Usage per Node")
    ax.legend(loc="upper right")
    _save(fig, out / "cpu", fmt)


def plot_throughput(by_node: dict, t0: float, out: Path, fmt: str) -> None:
    nodes = [n for n, recs in by_node.items()
             if any(r.get("net_tx_bytes") is not None for r in recs)]
    if not nodes:
        print("  [skip] throughput: no data (DB_ONLY mode?)")
        return
    fig, (ax_tx, ax_rx) = plt.subplots(1, 2, figsize=(10, 3))
    for i, node in enumerate(sorted(nodes)):
        recs = by_node[node]
        color = COLORS[i % len(COLORS)]
        for ax, key in [
            (ax_tx, "net_tx_bytes"),
            (ax_rx, "net_rx_bytes"),
        ]:
            times, rates = _rate(recs, key)
            rel = [t - t0 for t in times]
            kbps = [r / 1024 for r in rates]
            ax.plot(rel, kbps, label=node, color=color)
    for ax, title in [(ax_tx, "TX Throughput"), (ax_rx, "RX Throughput")]:
        ax.set_xlabel("Time (s)")
        ax.set_ylabel("KB/s")
        ax.set_title(title)
        ax.legend(loc="upper right")
    fig.tight_layout()
    _save(fig, out / "throughput", fmt)


def plot_memory(by_node: dict, t0: float, out: Path, fmt: str) -> None:
    nodes = [n for n, recs in by_node.items()
             if any(r.get("mem_rss_bytes") is not None for r in recs)]
    if not nodes:
        print("  [skip] memory: no data (DB_ONLY mode?)")
        return
    fig, ax = plt.subplots(figsize=(6, 3))
    for i, node in enumerate(sorted(nodes)):
        recs = by_node[node]
        ts   = _rel_times(recs, t0)
        vals = [v / (1024 ** 2) if v is not None else None
                for v in _values(recs, "mem_rss_bytes")]
        pairs = [(t, v) for t, v in zip(ts, vals) if v is not None]
        if pairs:
            ax.plot(*zip(*pairs), label=node, color=COLORS[i % len(COLORS)])
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Memory RSS (MiB)")
    ax.set_title("Memory Usage per Node")
    ax.legend(loc="upper right")
    _save(fig, out / "memory", fmt)


def plot_db(by_node: dict, t0: float, out: Path, fmt: str) -> None:
    nodes_count = [n for n, recs in by_node.items()
                   if any(r.get("db_stored_packets") is not None for r in recs)]
    if not nodes_count:
        print("  [skip] db: no data (no relay nodes or PLAN_NAME not set?)")
        return

    fig, ax = plt.subplots(figsize=(6, 3))
    for i, node in enumerate(sorted(nodes_count)):
        recs = by_node[node]
        ts   = _rel_times(recs, t0)
        vals = _values(recs, "db_stored_packets")
        pairs = [(t, v) for t, v in zip(ts, vals) if v is not None]
        if pairs:
            ax.plot(*zip(*pairs), label=node, color=COLORS[i % len(COLORS)])
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Stored Packets")
    ax.set_title("DTN Storage — Queued Packets per Relay")
    ax.yaxis.set_major_formatter(mticker.FormatStrFormatter("%d"))
    ax.legend(loc="upper right")
    _save(fig, out / "db_packets", fmt)

    nodes_avg = [n for n, recs in by_node.items()
                 if any(r.get("db_avg_delivery_sec") is not None for r in recs)]
    if not nodes_avg:
        return
    fig, ax = plt.subplots(figsize=(6, 3))
    for i, node in enumerate(sorted(nodes_avg)):
        recs = by_node[node]
        ts   = _rel_times(recs, t0)
        vals = _values(recs, "db_avg_delivery_sec")
        pairs = [(t, v) for t, v in zip(ts, vals) if v is not None]
        if pairs:
            ax.plot(*zip(*pairs), label=node, color=COLORS[i % len(COLORS)])
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Avg Delivery Time (s)")
    ax.set_title("DTN Storage — Average Delivery Time per Relay")
    ax.legend(loc="upper right")
    _save(fig, out / "db_delivery", fmt)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    p = argparse.ArgumentParser(
        description="Generate paper-quality plots from metrics.jsonl",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--captures", required=True,
                   help="Capture directory containing metrics.jsonl")
    p.add_argument("--out",      default="",
                   help="Output directory (default: <captures>/plots)")
    p.add_argument("--fmt",      default="svg",
                   choices=["pdf", "svg", "png"],
                   help="Output format (default: svg)")
    args = p.parse_args()

    captures = Path(args.captures)
    jsonl    = captures / "metrics.jsonl"
    if not jsonl.exists():
        raise SystemExit(f"ERROR: {jsonl} not found.\n"
                         "       Run an experiment first or check --captures path.")

    out_dir = Path(args.out) if args.out else captures / "plots"
    out_dir.mkdir(parents=True, exist_ok=True)

    print(f"[plot] reading  : {jsonl}")
    print(f"[plot] output   : {out_dir}  (format: {args.fmt})")

    by_node = load(jsonl)
    if not by_node:
        raise SystemExit("ERROR: metrics.jsonl is empty or malformed.")

    t0 = _global_t0(by_node)
    total_s = max(r["ts"] for recs in by_node.values() for r in recs) - t0
    print(f"[plot] nodes    : {sorted(by_node)}")
    print(f"[plot] duration : {total_s:.0f}s")
    print()

    plot_cpu(by_node, t0, out_dir, args.fmt)
    plot_throughput(by_node, t0, out_dir, args.fmt)
    plot_memory(by_node, t0, out_dir, args.fmt)
    plot_db(by_node, t0, out_dir, args.fmt)

    print()
    print(f"Done. {len(list(out_dir.glob('*.' + args.fmt)))} file(s) in {out_dir}/")


if __name__ == "__main__":
    main()
