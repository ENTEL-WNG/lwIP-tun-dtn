#!/usr/bin/env python3
"""
plot_metrics.py — Generate publication-quality line graphs from metrics.jsonl.

Reads <captures>/metrics.jsonl written by get_metrics during the experiment.

Usage:
  python3 plot_metrics.py --captures networks/<plan>/captures/<N> [options]

Options:
  --captures DIR    Capture directory containing metrics.jsonl (required)
  --out DIR         Output directory for plots (default: <captures>/plots)
  --fmt FORMAT      pdf, svg, or png (default: svg)
  --no-plots        Skip plot generation, write CSVs only

JSONL record fields:
  ts                   Unix timestamp
  node                 container name
  cpu_pct              CPU % (0–100 × ncores)
  mem_usage_bytes      memory usage
  mem_limit_bytes      memory limit
  net_rx_bytes         cumulative RX bytes (all ifaces)
  net_tx_bytes         cumulative TX bytes (all ifaces)
  net_ifaces           per-interface counters
  blk_read_bytes       cumulative block-device read bytes
  blk_write_bytes      cumulative block-device write bytes
  pids                 current PID / thread count
  db_stored_packets    rows in stored_packets table (null if no DB)
  db_avg_delivery_sec  AVG(delivery_time_in_sec) (null if no DB)
"""

import argparse
import json
import re
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt
import matplotlib.ticker as mticker

# ---------------------------------------------------------------------------
# Matplotlib style
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
    """Read metrics.jsonl and return {node: [record, ...]} sorted by timestamp."""
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


def _node_key(name: str):
    """Sort by the first integer found in the name so node10 > node3."""
    m = re.search(r"\d+", name)
    return (int(m.group()), name) if m else (0, name)


def _global_t0(by_node: dict) -> float:
    return min(r["ts"] for recs in by_node.values() for r in recs)


def _rel(records: list[dict], t0: float) -> list[float]:
    return [r["ts"] - t0 for r in records]


def _vals(records: list[dict], key: str) -> list:
    return [r.get(key) for r in records]


def _rate(records: list[dict], key: str) -> tuple[list[float], list[float]]:
    """Differentiate a cumulative counter → units/sec."""
    times, rates = [], []
    for i in range(1, len(records)):
        dt = records[i]["ts"] - records[i - 1]["ts"]
        v0, v1 = records[i - 1].get(key), records[i].get(key)
        if dt > 0 and v0 is not None and v1 is not None:
            times.append(records[i]["ts"])
            rates.append(max(0.0, (v1 - v0) / dt))
    return times, rates


def _iface_rate(records: list[dict], iface: str, key: str) -> tuple[list[float], list[float]]:
    """Differentiate a per-interface cumulative counter → units/sec."""
    times, rates = [], []
    for i in range(1, len(records)):
        dt = records[i]["ts"] - records[i - 1]["ts"]
        v0 = (records[i - 1].get("net_ifaces") or {}).get(iface, {}).get(key)
        v1 = (records[i].get("net_ifaces") or {}).get(iface, {}).get(key)
        if dt > 0 and v0 is not None and v1 is not None:
            times.append(records[i]["ts"])
            rates.append(max(0.0, (v1 - v0) / dt))
    return times, rates


_IFACE_RE = re.compile(r"^tun\d*$|^node\d+-node\d+$")


def _dtn_ifaces(records: list[dict]) -> list[str]:
    names: set[str] = set()
    for r in records:
        for iface in (r.get("net_ifaces") or {}):
            if _IFACE_RE.match(iface):
                names.add(iface)
    return sorted(names, key=_node_key)


# ---------------------------------------------------------------------------
# Plot helpers
# ---------------------------------------------------------------------------

def _save(fig: plt.Figure, path: Path, fmt: str) -> None:
    out = path.with_suffix(f".{fmt}")
    fig.savefig(out)
    plt.close(fig)
    print(f"  wrote {out}")


def _has(by_node: dict, key: str) -> list[str]:
    return [n for n, recs in by_node.items()
            if any(r.get(key) is not None for r in recs)]


# ---------------------------------------------------------------------------
# Plots
# ---------------------------------------------------------------------------

def plot_cpu(by_node: dict, t0: float, out: Path, fmt: str) -> None:
    nodes = _has(by_node, "cpu_pct")
    if not nodes:
        print("  [skip] cpu: no data")
        return
    fig, ax = plt.subplots(figsize=(6, 3))
    for i, node in enumerate(sorted(nodes, key=_node_key)):
        recs = by_node[node]
        ts   = _rel(recs, t0)
        vals = _vals(recs, "cpu_pct")
        pairs = [(t, v) for t, v in zip(ts, vals) if v is not None]
        if pairs:
            ax.plot(*zip(*pairs), label=node, color=COLORS[i % len(COLORS)])
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("CPU (%)")
    ax.set_title("CPU Usage per Node")
    ax.legend(loc="upper right")
    _save(fig, out / "cpu", fmt)


def plot_memory(by_node: dict, t0: float, out: Path, fmt: str) -> None:
    nodes = _has(by_node, "mem_usage_bytes")
    if not nodes:
        print("  [skip] memory: no data")
        return
    fig, ax = plt.subplots(figsize=(6, 3))
    for i, node in enumerate(sorted(nodes, key=_node_key)):
        recs  = by_node[node]
        ts    = _rel(recs, t0)
        usage = [v / (1024 ** 2) if v is not None else None
                 for v in _vals(recs, "mem_usage_bytes")]
        color = COLORS[i % len(COLORS)]
        pairs = [(t, v) for t, v in zip(ts, usage) if v is not None]
        if pairs:
            ax.plot(*zip(*pairs), label=node, color=color)
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Memory (MiB)")
    ax.set_title("Memory Usage per Node")
    ax.legend(loc="upper right")
    _save(fig, out / "memory", fmt)


def plot_throughput(by_node: dict, t0: float, out: Path, fmt: str) -> None:
    nodes = _has(by_node, "net_tx_bytes")
    if not nodes:
        print("  [skip] throughput: no data")
        return
    fig, (ax_tx, ax_rx) = plt.subplots(1, 2, figsize=(10, 3))
    for i, node in enumerate(sorted(nodes, key=_node_key)):
        recs  = by_node[node]
        color = COLORS[i % len(COLORS)]
        for ax, key in [(ax_tx, "net_tx_bytes"), (ax_rx, "net_rx_bytes")]:
            times, rates = _rate(recs, key)
            rel  = [t - t0 for t in times]
            kbps = [r / 1024 for r in rates]
            ax.plot(rel, kbps, label=node, color=color)
    for ax, title in [(ax_tx, "TX Throughput"), (ax_rx, "RX Throughput")]:
        ax.set_xlabel("Time (s)")
        ax.set_ylabel("KB/s")
        ax.set_title(title)
        ax.legend(loc="upper right")
    fig.tight_layout()
    _save(fig, out / "throughput", fmt)


def plot_blkio(by_node: dict, t0: float, out: Path, fmt: str) -> None:
    nodes = _has(by_node, "blk_write_bytes")
    if not nodes:
        print("  [skip] blkio: no data")
        return
    fig, (ax_rd, ax_wr) = plt.subplots(1, 2, figsize=(10, 3))
    for i, node in enumerate(sorted(nodes, key=_node_key)):
        recs  = by_node[node]
        color = COLORS[i % len(COLORS)]
        for ax, key in [(ax_rd, "blk_read_bytes"), (ax_wr, "blk_write_bytes")]:
            times, rates = _rate(recs, key)
            rel  = [t - t0 for t in times]
            kbps = [r / 1024 for r in rates]
            ax.plot(rel, kbps, label=node, color=color)
    for ax, title in [(ax_rd, "Block Read"), (ax_wr, "Block Write")]:
        ax.set_xlabel("Time (s)")
        ax.set_ylabel("KB/s")
        ax.set_title(title)
        ax.legend(loc="upper right")
    fig.tight_layout()
    _save(fig, out / "blkio", fmt)


def plot_iface_bytes(by_node: dict, t0: float, out: Path, fmt: str) -> None:
    series = [(node, iface)
              for node in sorted(by_node, key=_node_key)
              for iface in _dtn_ifaces(by_node[node])]
    if not series:
        print("  [skip] iface_bytes: no per-interface data")
        return
    fig, (ax_tx, ax_rx) = plt.subplots(1, 2, figsize=(10, 3))
    for i, (node, iface) in enumerate(series):
        recs  = by_node[node]
        color = COLORS[i % len(COLORS)]
        label = f"{node}/{iface}"
        for ax, key in [(ax_tx, "tx_bytes"), (ax_rx, "rx_bytes")]:
            times, rates = _iface_rate(recs, iface, key)
            rel  = [t - t0 for t in times]
            kbps = [r / 1024 for r in rates]
            ax.plot(rel, kbps, label=label, color=color)
    for ax, title in [(ax_tx, "TX per Interface"), (ax_rx, "RX per Interface")]:
        ax.set_xlabel("Time (s)")
        ax.set_ylabel("KB/s")
        ax.set_title(title)
        ax.legend(loc="upper right", fontsize=7)
    fig.tight_layout()
    _save(fig, out / "iface_bytes", fmt)


def plot_iface_packets(by_node: dict, t0: float, out: Path, fmt: str) -> None:
    series = [(node, iface)
              for node in sorted(by_node, key=_node_key)
              for iface in _dtn_ifaces(by_node[node])]
    if not series:
        print("  [skip] iface_packets: no per-interface data")
        return
    fig, (ax_tx, ax_rx) = plt.subplots(1, 2, figsize=(10, 3))
    for i, (node, iface) in enumerate(series):
        recs  = by_node[node]
        color = COLORS[i % len(COLORS)]
        label = f"{node}/{iface}"
        for ax, key in [(ax_tx, "tx_packets"), (ax_rx, "rx_packets")]:
            times, rates = _iface_rate(recs, iface, key)
            rel = [t - t0 for t in times]
            ax.plot(rel, rates, label=label, color=color)
    for ax, title in [(ax_tx, "TX Packets/s per Interface"), (ax_rx, "RX Packets/s per Interface")]:
        ax.set_xlabel("Time (s)")
        ax.set_ylabel("Packets/s")
        ax.set_title(title)
        ax.legend(loc="upper right", fontsize=7)
    fig.tight_layout()
    _save(fig, out / "iface_packets", fmt)


def plot_db(by_node: dict, t0: float, out: Path, fmt: str) -> None:
    nodes = _has(by_node, "db_stored_packets")
    if not nodes:
        print("  [skip] db: no data (no relay nodes or PLAN_NAME not set?)")
        return
    fig, ax = plt.subplots(figsize=(6, 3))
    for i, node in enumerate(sorted(nodes, key=_node_key)):
        recs = by_node[node]
        ts   = _rel(recs, t0)
        vals = _vals(recs, "db_stored_packets")
        pairs = [(t, v) for t, v in zip(ts, vals) if v is not None]
        if pairs:
            ax.plot(*zip(*pairs), label=node, color=COLORS[i % len(COLORS)])
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Stored Packets")
    ax.set_title("DTN Storage — Queued Packets per Relay")
    ax.yaxis.set_major_formatter(mticker.FormatStrFormatter("%d"))
    ax.legend(loc="upper right")
    _save(fig, out / "db_packets", fmt)



# ---------------------------------------------------------------------------
# CSV export
# ---------------------------------------------------------------------------

def _csv(path: Path, header: str, rows: list) -> None:
    with open(path, "w") as f:
        f.write(header + "\n")
        for row in rows:
            f.write(",".join(str(v) for v in row) + "\n")
    print(f"  wrote {path}")


def write_csvs(by_node: dict, t0: float, out: Path) -> None:
    csv_dir = out / "csv"
    csv_dir.mkdir(exist_ok=True)

    for node in sorted(by_node, key=_node_key):
        recs = by_node[node]

        rows = [(round(r["ts"] - t0, 3), round(r["cpu_pct"], 3))
                for r in recs if r.get("cpu_pct") is not None]
        if rows:
            _csv(csv_dir / f"cpu_{node}.csv", "time,cpu_pct", rows)

        rows = [(round(r["ts"] - t0, 3), round(r["mem_usage_bytes"] / 1024 ** 2, 3))
                for r in recs if r.get("mem_usage_bytes") is not None]
        if rows:
            _csv(csv_dir / f"memory_{node}.csv", "time,mem_usage_mib", rows)

        for direction, key in [("tx", "net_tx_bytes"), ("rx", "net_rx_bytes")]:
            times, rates = _rate(recs, key)
            rows = [(round(t - t0, 3), round(r / 1024, 3)) for t, r in zip(times, rates)]
            if rows:
                _csv(csv_dir / f"throughput_{direction}_{node}.csv",
                     f"time,{direction}_kbps", rows)

        for direction, key in [("read", "blk_read_bytes"), ("write", "blk_write_bytes")]:
            times, rates = _rate(recs, key)
            rows = [(round(t - t0, 3), round(r / 1024, 3)) for t, r in zip(times, rates)]
            if rows:
                _csv(csv_dir / f"blkio_{direction}_{node}.csv",
                     f"time,{direction}_kbps", rows)

        rows = [(round(r["ts"] - t0, 3), r["db_stored_packets"])
                for r in recs if r.get("db_stored_packets") is not None]
        if rows:
            _csv(csv_dir / f"db_packets_{node}.csv", "time,stored_packets", rows)

        for iface in _dtn_ifaces(recs):
            for direction, key in [("tx", "tx_packets"), ("rx", "rx_packets")]:
                times, rates = _iface_rate(recs, iface, key)
                rows = [(round(t - t0, 3), round(r, 3)) for t, r in zip(times, rates)]
                if rows:
                    _csv(csv_dir / f"iface_pkts_{direction}_{node}_{iface}.csv",
                         f"time,{direction}_pkts_per_sec", rows)
            for direction, key in [("tx", "tx_bytes"), ("rx", "rx_bytes")]:
                times, rates = _iface_rate(recs, iface, key)
                rows = [(round(t - t0, 3), round(r / 1024, 3)) for t, r in zip(times, rates)]
                if rows:
                    _csv(csv_dir / f"iface_kbps_{direction}_{node}_{iface}.csv",
                         f"time,{direction}_kbps", rows)


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
    p.add_argument("--no-plots", action="store_true",
                   help="Skip plot generation, write CSVs only")
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

    t0       = _global_t0(by_node)
    total_s  = max(r["ts"] for recs in by_node.values() for r in recs) - t0
    print(f"[plot] nodes    : {sorted(by_node, key=_node_key)}")
    print(f"[plot] duration : {total_s:.0f}s")
    print()

    if not args.no_plots:
        plot_cpu(by_node, t0, out_dir, args.fmt)
        plot_memory(by_node, t0, out_dir, args.fmt)
        plot_throughput(by_node, t0, out_dir, args.fmt)
        plot_blkio(by_node, t0, out_dir, args.fmt)
        plot_iface_bytes(by_node, t0, out_dir, args.fmt)
        plot_iface_packets(by_node, t0, out_dir, args.fmt)
        plot_db(by_node, t0, out_dir, args.fmt)
        print()

    write_csvs(by_node, t0, out_dir)

    print()
    print(f"Done. Output in {out_dir}/")


if __name__ == "__main__":
    main()
