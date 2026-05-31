#!/usr/bin/env python3
"""
analyze.py — Post-run analysis for DTN throughput experiments.

Reads the capture directory produced by a throughput test run and computes:
  - Packet Delivery Ratio (PDR)
  - End-to-end latency statistics (min / mean / p50 / p95 / p99 / max)
  - Throughput time-series (packets/s and kbit/s in 1-second bins)
  - Per-node CPU % and memory over time
  - DB storage count over time
  - [STATS] counter timeline parsed from docker logs
  - ICMPv6 200-203 event counts parsed from docker logs

Input files expected in <capture_dir>:
  sent.csv        — seq, sent_ts_us, size
  recv.csv        — seq, recv_ts_us
  metrics.jsonl   — per-second docker stats + DB snapshots (from monitor.py)
  logs.txt        — docker compose logs (contains [STATS] and ICMPv6 lines)

Output:
  <capture_dir>/report.json   — machine-readable summary
  <capture_dir>/report.txt    — human-readable summary (always written)
  <capture_dir>/report.html   — charts (written if matplotlib is installed)

Usage:
  python3 analyze.py <capture_dir>
"""

import argparse
import csv
import json
import os
import re
import sys
from collections import defaultdict


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def load_csv(path: str) -> list[dict]:
    if not os.path.exists(path):
        return []
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def load_jsonl(path: str) -> list[dict]:
    if not os.path.exists(path):
        return []
    records = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                try:
                    records.append(json.loads(line))
                except json.JSONDecodeError:
                    pass
    return records


def load_text(path: str) -> list[str]:
    if not os.path.exists(path):
        return []
    with open(path, errors="replace") as f:
        return f.readlines()


def percentile(sorted_values: list, p: float) -> float:
    if not sorted_values:
        return float("nan")
    idx = (len(sorted_values) - 1) * p / 100.0
    lo = int(idx)
    hi = min(lo + 1, len(sorted_values) - 1)
    frac = idx - lo
    return sorted_values[lo] * (1 - frac) + sorted_values[hi] * frac


# ---------------------------------------------------------------------------
# Traffic analysis
# ---------------------------------------------------------------------------

def analyze_traffic(sent: list[dict], recv: list[dict]) -> dict:
    if not sent:
        return {"error": "sent.csv missing or empty"}

    sent_map = {int(r["seq"]): int(r["sent_ts_us"]) for r in sent}
    recv_map = {int(r["seq"]): int(r["recv_ts_us"]) for r in recv}

    n_sent = len(sent_map)
    n_recv = len(recv_map)
    n_delivered = len(sent_map.keys() & recv_map.keys())

    pdr = n_delivered / n_sent if n_sent else 0.0

    latencies_us = sorted(
        recv_map[seq] - sent_map[seq]
        for seq in sent_map.keys() & recv_map.keys()
    )

    latency_stats = {}
    if latencies_us:
        latency_stats = {
            "min_ms":  latencies_us[0]  / 1000,
            "mean_ms": sum(latencies_us) / len(latencies_us) / 1000,
            "p50_ms":  percentile(latencies_us, 50) / 1000,
            "p95_ms":  percentile(latencies_us, 95) / 1000,
            "p99_ms":  percentile(latencies_us, 99) / 1000,
            "max_ms":  latencies_us[-1] / 1000,
        }

    # Throughput binned by second (using recv timestamp)
    throughput_bins: dict[int, dict] = defaultdict(lambda: {"pkts": 0, "bits": 0})
    size_map = {int(r["seq"]): int(r["size"]) for r in sent}
    for seq, recv_ts in recv_map.items():
        bin_sec = recv_ts // 1_000_000
        throughput_bins[bin_sec]["pkts"] += 1
        throughput_bins[bin_sec]["bits"] += size_map.get(seq, 0) * 8

    throughput_ts = sorted(throughput_bins.items())

    return {
        "n_sent": n_sent,
        "n_recv": n_recv,
        "n_delivered": n_delivered,
        "pdr": round(pdr, 4),
        "latency": latency_stats,
        "throughput_bins": [
            {"ts": ts, "pkts_per_sec": v["pkts"], "kbps": v["bits"] / 1000}
            for ts, v in throughput_ts
        ],
    }


# ---------------------------------------------------------------------------
# Metrics analysis
# ---------------------------------------------------------------------------

def analyze_metrics(metrics: list[dict]) -> dict:
    if not metrics:
        return {"error": "metrics.jsonl missing or empty"}

    nodes = sorted({m["node"] for m in metrics})
    result = {}

    for node in nodes:
        rows = [m for m in metrics if m["node"] == node]
        cpu_vals = [r["cpu_pct"] for r in rows if r.get("cpu_pct", -1) >= 0]
        mem_vals = [r["mem_mb"]  for r in rows if r.get("mem_mb",  -1) >= 0]
        db_vals  = [r["db_count"] for r in rows if r.get("db_count", -1) >= 0]

        result[node] = {
            "cpu_mean_pct": round(sum(cpu_vals) / len(cpu_vals), 2) if cpu_vals else -1,
            "cpu_max_pct":  round(max(cpu_vals), 2) if cpu_vals else -1,
            "mem_mean_mb":  round(sum(mem_vals) / len(mem_vals), 2) if mem_vals else -1,
            "mem_max_mb":   round(max(mem_vals), 2) if mem_vals else -1,
            "db_max_count": max(db_vals) if db_vals else -1,
            "samples": len(rows),
        }

    return result


# ---------------------------------------------------------------------------
# Log parsing
# ---------------------------------------------------------------------------

# [STATS] fwd_now=X fwd_stored=X stored=X dropped=X db=N
_STATS_RE = re.compile(
    r"\[STATS\]\s+fwd_now=(\d+)\s+fwd_stored=(\d+)\s+stored=(\d+)\s+dropped=(\d+)\s+db=(-?\d+)"
)
# ICMPv6 200-203 events
_ICMP_RE = re.compile(r"ICMPv6\|\|.*?type\s+(\d+)")


def analyze_logs(lines: list[str]) -> dict:
    stats_snapshots = []
    icmpv6_counts = defaultdict(int)

    for line in lines:
        m = _STATS_RE.search(line)
        if m:
            stats_snapshots.append({
                "fwd_now":    int(m.group(1)),
                "fwd_stored": int(m.group(2)),
                "stored":     int(m.group(3)),
                "dropped":    int(m.group(4)),
                "db":         int(m.group(5)),
            })

        m2 = _ICMP_RE.search(line)
        if m2:
            t = int(m2.group(1))
            icmpv6_counts[t] += 1

    final = stats_snapshots[-1] if stats_snapshots else {}

    return {
        "stats_snapshots": len(stats_snapshots),
        "final_counters": final,
        "icmpv6_events": {
            "RECEIVED (200)":  icmpv6_counts.get(200, 0),
            "FORWARDED (201)": icmpv6_counts.get(201, 0),
            "DELIVERED (202)": icmpv6_counts.get(202, 0),
            "DELETED (203)":   icmpv6_counts.get(203, 0),
        },
    }


# ---------------------------------------------------------------------------
# Text report
# ---------------------------------------------------------------------------

def make_text_report(traffic: dict, resources: dict, logs: dict) -> str:
    lines = []
    lines.append("=" * 60)
    lines.append("DTN THROUGHPUT EXPERIMENT REPORT")
    lines.append("=" * 60)

    lines.append("\n--- Traffic ---")
    if "error" not in traffic:
        lines.append(f"  Sent:      {traffic['n_sent']}")
        lines.append(f"  Received:  {traffic['n_recv']}")
        lines.append(f"  Delivered: {traffic['n_delivered']}")
        lines.append(f"  PDR:       {traffic['pdr'] * 100:.1f}%")
        lat = traffic.get("latency", {})
        if lat:
            lines.append(f"  Latency (ms): min={lat['min_ms']:.1f}  "
                         f"mean={lat['mean_ms']:.1f}  "
                         f"p95={lat['p95_ms']:.1f}  "
                         f"max={lat['max_ms']:.1f}")
        bins = traffic.get("throughput_bins", [])
        if bins:
            avg_pps = sum(b["pkts_per_sec"] for b in bins) / len(bins)
            avg_kbps = sum(b["kbps"] for b in bins) / len(bins)
            lines.append(f"  Avg throughput: {avg_pps:.1f} pkt/s  {avg_kbps:.1f} kbit/s")
    else:
        lines.append(f"  {traffic['error']}")

    lines.append("\n--- Resource Usage (per node) ---")
    if "error" not in resources:
        for node, r in resources.items():
            lines.append(f"  {node}:  CPU mean={r['cpu_mean_pct']}%  max={r['cpu_max_pct']}%  "
                         f"Mem mean={r['mem_mean_mb']} MiB  max={r['mem_max_mb']} MiB  "
                         f"DB peak={r['db_max_count']} pkts")
    else:
        lines.append(f"  {resources['error']}")

    lines.append("\n--- Controller Counters (from logs) ---")
    fc = logs.get("final_counters", {})
    if fc:
        lines.append(f"  fwd_now={fc.get('fwd_now', '?')}  "
                     f"fwd_stored={fc.get('fwd_stored', '?')}  "
                     f"stored={fc.get('stored', '?')}  "
                     f"dropped={fc.get('dropped', '?')}  "
                     f"db={fc.get('db', '?')}")
    else:
        lines.append("  (no [STATS] lines found in logs.txt)")

    lines.append("\n--- ICMPv6 DTN Events ---")
    for name, count in logs.get("icmpv6_events", {}).items():
        lines.append(f"  {name}: {count}")

    lines.append("")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Optional HTML report with matplotlib
# ---------------------------------------------------------------------------

def make_html_report(traffic: dict, resources: dict, capture_dir: str) -> str:
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import io, base64
    except ImportError:
        return ""

    figs_b64 = []

    # Throughput time-series
    bins = traffic.get("throughput_bins", [])
    if bins:
        ts0 = bins[0]["ts"]
        xs = [b["ts"] - ts0 for b in bins]
        ys = [b["pkts_per_sec"] for b in bins]
        fig, ax = plt.subplots(figsize=(8, 3))
        ax.bar(xs, ys, width=0.8)
        ax.set_xlabel("Time (s)")
        ax.set_ylabel("Packets / s")
        ax.set_title("Receive throughput")
        buf = io.BytesIO()
        fig.savefig(buf, format="png", bbox_inches="tight")
        figs_b64.append(base64.b64encode(buf.getvalue()).decode())
        plt.close(fig)

    # CPU per node
    metrics = load_jsonl(os.path.join(capture_dir, "metrics.jsonl"))
    if metrics:
        nodes = sorted({m["node"] for m in metrics})
        fig, ax = plt.subplots(figsize=(8, 3))
        t0 = metrics[0]["ts"]
        for node in nodes:
            rows = [m for m in metrics if m["node"] == node]
            xs = [r["ts"] - t0 for r in rows]
            ys = [r.get("cpu_pct", 0) for r in rows]
            ax.plot(xs, ys, label=node)
        ax.set_xlabel("Time (s)")
        ax.set_ylabel("CPU %")
        ax.set_title("CPU usage per node")
        ax.legend()
        buf = io.BytesIO()
        fig.savefig(buf, format="png", bbox_inches="tight")
        figs_b64.append(base64.b64encode(buf.getvalue()).decode())
        plt.close(fig)

        # DB count for DTN nodes
        dtn_rows = [m for m in metrics if m.get("db_count", -1) >= 0]
        if dtn_rows:
            fig, ax = plt.subplots(figsize=(8, 3))
            for node in nodes:
                rows = [m for m in dtn_rows if m["node"] == node and m.get("db_count", -1) >= 0]
                if rows:
                    xs = [r["ts"] - t0 for r in rows]
                    ys = [r["db_count"] for r in rows]
                    ax.plot(xs, ys, label=node)
            ax.set_xlabel("Time (s)")
            ax.set_ylabel("DB row count")
            ax.set_title("Stored packets in DB over time")
            ax.legend()
            buf = io.BytesIO()
            fig.savefig(buf, format="png", bbox_inches="tight")
            figs_b64.append(base64.b64encode(buf.getvalue()).decode())
            plt.close(fig)

    imgs = "\n".join(
        f'<img src="data:image/png;base64,{b}" style="max-width:100%;margin:10px 0"><br>'
        for b in figs_b64
    )
    return f"<html><body style='font-family:sans-serif'>{imgs}</body></html>"


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    p = argparse.ArgumentParser(description="Analyze a DTN experiment capture directory")
    p.add_argument("capture_dir", help="Path to capture directory")
    args = p.parse_args()

    d = args.capture_dir

    sent    = load_csv(os.path.join(d, "sent.csv"))
    recv    = load_csv(os.path.join(d, "recv.csv"))
    metrics = load_jsonl(os.path.join(d, "metrics.jsonl"))
    logs    = load_text(os.path.join(d, "logs.txt"))

    traffic   = analyze_traffic(sent, recv)
    resources = analyze_metrics(metrics)
    log_data  = analyze_logs(logs)

    summary = {
        "traffic":   traffic,
        "resources": resources,
        "logs":      log_data,
    }

    report_json = os.path.join(d, "report.json")
    with open(report_json, "w") as f:
        json.dump(summary, f, indent=2)
    print(f"[analyze] wrote {report_json}")

    report_txt = os.path.join(d, "report.txt")
    txt = make_text_report(traffic, resources, log_data)
    with open(report_txt, "w") as f:
        f.write(txt)
    print(txt)
    print(f"[analyze] wrote {report_txt}")

    html = make_html_report(traffic, resources, d)
    if html:
        report_html = os.path.join(d, "report.html")
        with open(report_html, "w") as f:
            f.write(html)
        print(f"[analyze] wrote {report_html}")
    else:
        print("[analyze] matplotlib not available — skipping report.html")


if __name__ == "__main__":
    main()
