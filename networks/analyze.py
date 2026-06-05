#!/usr/bin/env python3
"""
analyze.py — Post-run analysis for DTN throughput experiments.

Reads the capture directory produced by a throughput test run and computes:
  - Packet Delivery Ratio (PDR)
  - End-to-end latency statistics (min / mean / p50 / p95 / p99 / max)
  - Throughput time-series (packets/s and kbit/s in 1-second bins)
  - Per-node CPU %, memory, and network throughput
  - DB storage count over time
  - [STATS] counter timeline parsed from docker logs
  - ICMPv6 200-203 event counts parsed from docker logs

Input files expected in <capture_dir>:
  sent.csv        — seq, sent_ts_us, size
  recv.csv        — seq, recv_ts_us
  metrics.jsonl   — per-second docker stats + DB snapshots (from get_metrics.py)
  logs.txt        — docker compose logs (contains [STATS] and ICMPv6 lines)

Output:
  <capture_dir>/report.txt  — human-readable summary

Usage:
  python3 analyze.py <capture_dir>
"""

import argparse
import csv
import json
import os
import platform
import re
import shutil
import sys
from collections import defaultdict


# ---------------------------------------------------------------------------
# Hardware info
# ---------------------------------------------------------------------------

def collect_hardware(capture_dir: str) -> dict:
    hw: dict = {}

    # CPU
    cpu_model = "unknown"
    cpu_cores = os.cpu_count() or 0
    cpu_freq_mhz = None
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("model name") and cpu_model == "unknown":
                    cpu_model = line.split(":", 1)[1].strip()
                if line.startswith("cpu MHz") and cpu_freq_mhz is None:
                    try:
                        cpu_freq_mhz = float(line.split(":", 1)[1].strip())
                    except ValueError:
                        pass
    except OSError:
        pass
    hw["cpu"] = {
        "model":     cpu_model,
        "cores":     cpu_cores,
        "freq_mhz":  round(cpu_freq_mhz, 1) if cpu_freq_mhz else None,
    }

    # RAM
    ram_total_kb = None
    ram_avail_kb = None
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemTotal:"):
                    ram_total_kb = int(line.split()[1])
                elif line.startswith("MemAvailable:"):
                    ram_avail_kb = int(line.split()[1])
    except OSError:
        pass
    hw["ram"] = {
        "total_gib":  round(ram_total_kb / (1024 ** 2), 2) if ram_total_kb else None,
        "avail_gib":  round(ram_avail_kb / (1024 ** 2), 2) if ram_avail_kb else None,
    }

    # Storage (disk containing the capture directory)
    try:
        usage = shutil.disk_usage(capture_dir)
        hw["storage"] = {
            "total_gib": round(usage.total / (1024 ** 3), 2),
            "used_gib":  round(usage.used  / (1024 ** 3), 2),
            "free_gib":  round(usage.free  / (1024 ** 3), 2),
        }
    except OSError:
        hw["storage"] = {}

    # OS
    hw["os"] = {
        "system":  platform.system(),
        "release": platform.release(),
        "version": platform.version(),
        "machine": platform.machine(),
    }

    return hw


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
    lo  = int(idx)
    hi  = min(lo + 1, len(sorted_values) - 1)
    return sorted_values[lo] * (1 - (idx - lo)) + sorted_values[hi] * (idx - lo)


# ---------------------------------------------------------------------------
# Traffic analysis
# ---------------------------------------------------------------------------

def analyze_traffic(sent: list[dict], recv: list[dict]) -> dict:
    if not sent:
        return {"error": "sent.csv missing or empty"}

    sent_map = {int(r["seq"]): int(r["sent_ts_us"]) for r in sent}
    recv_map = {int(r["seq"]): int(r["recv_ts_us"]) for r in recv}
    size_map = {int(r["seq"]): int(r["size"]) for r in sent}

    n_sent     = len(sent_map)
    n_recv     = len(recv_map)
    n_delivered = len(sent_map.keys() & recv_map.keys())
    pdr        = n_delivered / n_sent if n_sent else 0.0

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

    throughput_bins: dict[int, dict] = defaultdict(lambda: {"pkts": 0, "bits": 0})
    for seq, recv_ts in recv_map.items():
        bin_sec = recv_ts // 1_000_000
        throughput_bins[bin_sec]["pkts"] += 1
        throughput_bins[bin_sec]["bits"] += size_map.get(seq, 0) * 8

    throughput_ts = sorted(throughput_bins.items())

    return {
        "n_sent":      n_sent,
        "n_recv":      n_recv,
        "n_delivered": n_delivered,
        "pdr":         round(pdr, 4),
        "latency":     latency_stats,
        "throughput_bins": [
            {"ts": ts, "pkts_per_sec": v["pkts"], "kbps": v["bits"] / 1000}
            for ts, v in throughput_ts
        ],
    }


# ---------------------------------------------------------------------------
# Metrics analysis
# ---------------------------------------------------------------------------

def _net_rates(rows: list[dict]) -> list[dict]:
    """Derive per-second RX/TX kbit/s from consecutive cumulative byte samples."""
    rates = []
    for i in range(1, len(rows)):
        prev, curr = rows[i - 1], rows[i]
        dt = curr["ts"] - prev["ts"]
        if dt <= 0:
            continue
        rx_p = prev.get("net_rx_bytes")
        rx_c = curr.get("net_rx_bytes")
        tx_p = prev.get("net_tx_bytes")
        tx_c = curr.get("net_tx_bytes")
        if any(v is None for v in (rx_p, rx_c, tx_p, tx_c)):
            continue
        if rx_c < rx_p or tx_c < tx_p:   # counter reset — skip
            continue
        rates.append({
            "ts":      curr["ts"],
            "rx_kbps": (rx_c - rx_p) / dt / 1024,
            "tx_kbps": (tx_c - tx_p) / dt / 1024,
        })
    return rates


def analyze_metrics(metrics: list[dict]) -> dict:
    if not metrics:
        return {"error": "metrics.jsonl missing or empty"}

    nodes  = sorted({m["node"] for m in metrics})
    result = {}

    for node in nodes:
        rows = [m for m in metrics if m["node"] == node]

        cpu_vals = [r["cpu_pct"] for r in rows if r.get("cpu_pct") is not None]
        mem_vals = [r["mem_usage_bytes"] / (1024 ** 2)
                    for r in rows if r.get("mem_usage_bytes") is not None]
        db_vals  = [r["db_stored_packets"]
                    for r in rows if r.get("db_stored_packets") is not None]

        rates   = _net_rates(rows)
        rx_vals = [r["rx_kbps"] for r in rates]
        tx_vals = [r["tx_kbps"] for r in rates]

        result[node] = {
            "cpu_mean_pct":     round(sum(cpu_vals) / len(cpu_vals), 2) if cpu_vals else None,
            "cpu_max_pct":      round(max(cpu_vals), 2)                 if cpu_vals else None,
            "mem_mean_mb":      round(sum(mem_vals) / len(mem_vals), 2) if mem_vals else None,
            "mem_max_mb":       round(max(mem_vals), 2)                 if mem_vals else None,
            "db_max_count":     max(db_vals)                            if db_vals  else None,
            "net_rx_kbps_mean": round(sum(rx_vals) / len(rx_vals), 2)  if rx_vals  else None,
            "net_rx_kbps_peak": round(max(rx_vals), 2)                 if rx_vals  else None,
            "net_tx_kbps_mean": round(sum(tx_vals) / len(tx_vals), 2)  if tx_vals  else None,
            "net_tx_kbps_peak": round(max(tx_vals), 2)                 if tx_vals  else None,
            "samples":          len(rows),
        }

    return result


# ---------------------------------------------------------------------------
# Log parsing
# ---------------------------------------------------------------------------

_STATS_RE = re.compile(
    r"\[STATS\]\s+fwd_now=(\d+)\s+fwd_stored=(\d+)\s+stored=(\d+)\s+dropped=(\d+)\s+db=(-?\d+)"
)
_ICMP_RE = re.compile(r"ICMPv6\|\|.*?type\s+(\d+)")


def analyze_logs(lines: list[str]) -> dict:
    stats_snapshots = []
    icmpv6_counts   = defaultdict(int)

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
            icmpv6_counts[int(m2.group(1))] += 1

    return {
        "stats_snapshots": len(stats_snapshots),
        "final_counters":  stats_snapshots[-1] if stats_snapshots else {},
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

def _fmt(v, fmt=".2f", unit="") -> str:
    return f"{v:{fmt}}{unit}" if v is not None else "n/a"


def make_text_report(traffic: dict, resources: dict, logs: dict, hw: dict) -> str:
    lines = []
    lines.append("=" * 60)
    lines.append("DTN THROUGHPUT EXPERIMENT REPORT")
    lines.append("=" * 60)

    lines.append("\n--- Hardware ---")
    cpu = hw.get("cpu", {})
    ram = hw.get("ram", {})
    sto = hw.get("storage", {})
    oss = hw.get("os", {})
    freq_str = f"  {cpu['freq_mhz']} MHz" if cpu.get('freq_mhz') else ""
    lines.append(f"  CPU:     {cpu.get('model', 'unknown')}  "
                 f"({cpu.get('cores', '?')} cores{freq_str})")
    lines.append(f"  RAM:     {_fmt(ram.get('total_gib'), '.2f')} GiB total  "
                 f"{_fmt(ram.get('avail_gib'), '.2f')} GiB available")
    lines.append(f"  Storage: {_fmt(sto.get('total_gib'), '.1f')} GiB total  "
                 f"{_fmt(sto.get('used_gib'), '.1f')} used  "
                 f"{_fmt(sto.get('free_gib'), '.1f')} free")
    lines.append(f"  OS:      {oss.get('system', '?')} {oss.get('release', '')}  "
                 f"({oss.get('machine', '?')})")

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
            avg_pps  = sum(b["pkts_per_sec"] for b in bins) / len(bins)
            avg_kbps = sum(b["kbps"] for b in bins) / len(bins)
            lines.append(f"  Avg throughput: {avg_pps:.1f} pkt/s  {avg_kbps:.1f} kbit/s")
    else:
        lines.append(f"  {traffic['error']}")

    lines.append("\n--- Per-node Network Throughput ---")
    if "error" not in resources:
        for node, r in resources.items():
            rx_mean = r.get("net_rx_kbps_mean")
            rx_peak = r.get("net_rx_kbps_peak")
            tx_mean = r.get("net_tx_kbps_mean")
            tx_peak = r.get("net_tx_kbps_peak")
            if rx_mean is not None:
                lines.append(f"  {node}:  RX {rx_mean:.1f} KB/s avg  {rx_peak:.1f} peak"
                             f"  |  TX {tx_mean:.1f} KB/s avg  {tx_peak:.1f} peak")
            else:
                lines.append(f"  {node}:  (no net rate data)")
    else:
        lines.append(f"  {resources['error']}")

    lines.append("\n--- Per-node Resource Usage ---")
    if "error" not in resources:
        for node, r in resources.items():
            lines.append(
                f"  {node}:  CPU mean={_fmt(r['cpu_mean_pct'])}%  max={_fmt(r['cpu_max_pct'])}%"
                f"  |  Mem mean={_fmt(r['mem_mean_mb'])} MiB  max={_fmt(r['mem_max_mb'])} MiB"
                f"  |  DB peak={r['db_max_count'] if r['db_max_count'] is not None else 'n/a'}"
                f"  |  samples={r['samples']}"
            )
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
    hw        = collect_hardware(d)

    txt = make_text_report(traffic, resources, log_data, hw)
    print(txt)

    report_txt = os.path.join(d, "report.txt")
    with open(report_txt, "w") as f:
        f.write(txt)
    print(f"[analyze] wrote {report_txt}")


if __name__ == "__main__":
    main()
