#!/usr/bin/env python3
"""
dtn_exporter.py — Prometheus exporter for DTN node container stats.

Reads cgroup v2 and /proc files from inside each container via docker exec.
Works on Docker Desktop for macOS where the Docker stats API returns empty data.

One background thread per container measures CPU over a 1-second window then
immediately reads memory, network, and SQLite DB stats.
Prometheus scrapes the cached gauges.

Metrics on :9091/metrics:
  dtn_cpu_percent{container}           CPU %  (0–100 × ncores)
  dtn_memory_rss_bytes{container}      RSS (memory.current − file cache)
  dtn_memory_total_bytes{container}    Total memory (memory.current)
  dtn_net_rx_bytes_total{container}    cumulative RX bytes (excl. lo)
  dtn_net_tx_bytes_total{container}    cumulative TX bytes (excl. lo)
  dtn_db_stored_packets{container}     rows in stored_packets table
  dtn_db_avg_delivery_sec{container}   AVG(delivery_time_in_sec)
"""

import json
import os
import sqlite3
import threading
import time

import docker
from prometheus_client import Gauge, start_http_server

PORT        = int(os.environ.get("EXPORTER_PORT",    "9091"))
INTERVAL    = float(os.environ.get("SCRAPE_INTERVAL", "1"))
NODE_PREFIX = os.environ.get("NODE_PREFIX",           "node")
PLAN_NAME   = os.environ.get("PLAN_NAME",             "")
DB_DIR      = f"/repo/dtn_storage/{PLAN_NAME}" if PLAN_NAME else ""
# DB_ONLY=1: skip CPU/memory/network collection (set when cAdvisor handles those).
DB_ONLY     = os.environ.get("DB_ONLY", "0") == "1"
# METRICS_OUT: path to a JSONL file written each collection cycle.
# Allows post-run plotting without needing Prometheus running.
METRICS_OUT = os.environ.get("METRICS_OUT", "")

_jsonl_lock = threading.Lock()

cpu_gauge  = Gauge("dtn_cpu_percent",          "CPU usage %",                    ["container"])
mem_gauge  = Gauge("dtn_memory_rss_bytes",     "Memory RSS bytes (excl. cache)", ["container"])
mem_total  = Gauge("dtn_memory_total_bytes",   "Total memory usage bytes",       ["container"])
rx_gauge   = Gauge("dtn_net_rx_bytes_total",   "Cumulative RX bytes",            ["container"])
tx_gauge   = Gauge("dtn_net_tx_bytes_total",   "Cumulative TX bytes",            ["container"])
db_count   = Gauge("dtn_db_stored_packets",    "Rows in stored_packets table",   ["container"])
db_avg_del = Gauge("dtn_db_avg_delivery_sec",  "AVG delivery_time_in_sec",       ["container"])


def _exec(container, cmd: str) -> str:
    r = container.exec_run(cmd)
    return r.output.decode(errors="replace").strip()


def _cpu_usec(container) -> int:
    for line in _exec(container, "cat /sys/fs/cgroup/cpu.stat").splitlines():
        if line.startswith("usage_usec"):
            try:
                return int(line.split()[1])
            except (IndexError, ValueError):
                pass
    return 0


def _memory(container) -> tuple[int, int]:
    """Return (rss_bytes, total_bytes). rss = total − file cache."""
    try:
        current   = int(_exec(container, "cat /sys/fs/cgroup/memory.current"))
        stats     = {}
        for line in _exec(container, "cat /sys/fs/cgroup/memory.stat").splitlines():
            parts = line.split()
            if len(parts) == 2:
                try:
                    stats[parts[0]] = int(parts[1])
                except ValueError:
                    pass
        rss = max(0, current - stats.get("file", 0))
        return rss, current
    except Exception:
        return 0, 0


def _db_stats(name: str) -> tuple[int | None, float | None]:
    """Query stored_packets table.
    Returns (count, avg_delivery) when the DB exists, (None, None) when
    there is no DB for this node (not a DTN storage node for this plan)."""
    if not DB_DIR:
        return None, None
    db_path = f"{DB_DIR}/{name}_dtn_packets.db"
    if not os.path.exists(db_path):
        return None, None
    try:
        uri = f"file:{db_path}?mode=ro&nolock=1"
        con = sqlite3.connect(uri, uri=True, timeout=1)
        row = con.execute(
            "SELECT COUNT(*), AVG(best_delivery_time_in_sec) FROM stored_packets"
        ).fetchone()
        con.close()
        count = int(row[0])   if row[0] is not None else 0
        avg   = float(row[1]) if row[1] is not None else None
        return count, avg
    except Exception:
        return None, None


def _net_bytes(container) -> tuple[int, int]:
    rx = tx = 0
    try:
        for line in _exec(container, "cat /proc/net/dev").splitlines()[2:]:
            parts = line.split()
            if not parts:
                continue
            if parts[0].rstrip(":") == "lo":
                continue
            rx += int(parts[1])
            tx += int(parts[9])
    except Exception:
        pass
    return rx, tx


def _write_jsonl(record: dict) -> None:
    try:
        os.makedirs(os.path.dirname(METRICS_OUT), exist_ok=True)
        with _jsonl_lock:
            with open(METRICS_OUT, "a") as f:
                f.write(json.dumps(record) + "\n")
    except Exception as exc:
        print(f"[exporter] jsonl write error: {exc}", flush=True)


def _poll(container, stop: threading.Event) -> None:
    name = container.name
    mode = "db-only" if DB_ONLY else "full"
    print(f"[exporter] polling {name} ({mode})", flush=True)
    while not stop.is_set():
        t0 = time.monotonic()
        try:
            if not DB_ONLY:
                # CPU requires a 1-second window between two cgroup reads.
                u0 = _cpu_usec(container)
                stop.wait(1.0)
                if stop.is_set():
                    break
                u1      = _cpu_usec(container)
                elapsed = (time.monotonic() - t0) * 1e6
                cpu     = (u1 - u0) / elapsed * 100.0 if elapsed > 0 else 0.0
                cpu_gauge.labels(container=name).set(max(0.0, cpu))
                rss, total = _memory(container)
                mem_gauge.labels(container=name).set(rss)
                mem_total.labels(container=name).set(total)
                rx, tx = _net_bytes(container)
                rx_gauge.labels(container=name).set(rx)
                tx_gauge.labels(container=name).set(tx)

            count, avg = _db_stats(name)
            if count is not None:
                db_count.labels(container=name).set(count)
            if avg is not None:
                db_avg_del.labels(container=name).set(avg)

            if METRICS_OUT:
                record: dict = {"ts": time.time(), "node": name}
                if not DB_ONLY:
                    record.update({
                        "cpu_pct":        round(cpu, 3),
                        "mem_rss_bytes":  rss,
                        "mem_total_bytes": total,
                        "net_rx_bytes":   rx,
                        "net_tx_bytes":   tx,
                    })
                record["db_stored_packets"]    = count
                record["db_avg_delivery_sec"]  = avg
                _write_jsonl(record)

        except Exception as exc:
            print(f"[exporter] {name}: {exc}", flush=True)

        elapsed_total = time.monotonic() - t0
        stop.wait(max(0.0, INTERVAL - elapsed_total))


def main() -> None:
    start_http_server(PORT)
    print(f"[exporter] listening on :{PORT}  interval={INTERVAL}s", flush=True)

    if METRICS_OUT:
        os.makedirs(os.path.dirname(os.path.abspath(METRICS_OUT)), exist_ok=True)
        open(METRICS_OUT, "w").close()  # truncate so each run starts clean
        print(f"[exporter] metrics  → {METRICS_OUT}", flush=True)

    client = docker.from_env()
    stop   = threading.Event()

    containers = [c for c in client.containers.list()
                  if c.name.startswith(NODE_PREFIX)]
    print(f"[exporter] found {len(containers)} node container(s): "
          f"{[c.name for c in containers]}", flush=True)

    for c in containers:
        threading.Thread(target=_poll, args=(c, stop), daemon=True).start()

    try:
        while True:
            time.sleep(60)
    except KeyboardInterrupt:
        stop.set()


if __name__ == "__main__":
    main()
