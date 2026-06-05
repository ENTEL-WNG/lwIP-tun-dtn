#!/usr/bin/env python3
"""
dtn_exporter.py — Per-container metrics collector for DTN nodes.

Reads cgroup v2 and /proc files from inside each container via docker exec.
One background thread per container measures CPU over a 1-second window then
immediately reads memory, network, and SQLite DB stats, writing every sample
to a JSONL file.

JSONL record fields:
  ts                   Unix timestamp
  node                 container name
  cpu_pct              CPU % (0–100 × ncores)
  mem_rss_bytes        RSS (memory.current − file cache)
  mem_total_bytes      total memory (memory.current)
  net_rx_bytes         cumulative RX bytes (excl. lo, all ifaces)
  net_tx_bytes         cumulative TX bytes (excl. lo, all ifaces)
  net_ifaces           per-interface counters (see _net_iface_stats)
  db_stored_packets    rows in stored_packets table (null if no DB)
  db_avg_delivery_sec  AVG(delivery_time_in_sec) (null if no DB)
"""

import json
import os
import sqlite3
import threading
import time

import docker

INTERVAL    = float(os.environ.get("SCRAPE_INTERVAL", "0.1"))
NODE_PREFIX = os.environ.get("NODE_PREFIX",           "node")
PLAN_NAME   = os.environ.get("PLAN_NAME",             "")
DB_DIR      = f"/repo/dtn_storage/{PLAN_NAME}" if PLAN_NAME else ""
METRICS_OUT = os.environ.get("METRICS_OUT", "")

_jsonl_lock = threading.Lock()


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


def _net_iface_stats(container) -> dict[str, dict[str, int]]:
    """Parse /proc/net/dev and return per-interface counters (excl. lo).

    Returns {iface: {rx_bytes, rx_packets, rx_errors, rx_drop,
                     tx_bytes, tx_packets, tx_errors, tx_drop}}.
    """
    ifaces: dict[str, dict[str, int]] = {}
    try:
        for line in _exec(container, "cat /proc/net/dev").splitlines()[2:]:
            parts = line.split()
            if len(parts) < 17:
                continue
            name = parts[0].rstrip(":")
            if name == "lo":
                continue
            ifaces[name] = {
                "rx_bytes":   int(parts[1]),
                "rx_packets": int(parts[2]),
                "rx_errors":  int(parts[3]),
                "rx_drop":    int(parts[4]),
                "tx_bytes":   int(parts[9]),
                "tx_packets": int(parts[10]),
                "tx_errors":  int(parts[11]),
                "tx_drop":    int(parts[12]),
            }
    except Exception:
        pass
    return ifaces


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
    print(f"[exporter] polling {name}", flush=True)
    while not stop.is_set():
        t0 = time.monotonic()
        try:
            # CPU requires a 1-second window between two cgroup reads.
            u0 = _cpu_usec(container)
            stop.wait(1.0)
            if stop.is_set():
                break
            u1      = _cpu_usec(container)
            elapsed = (time.monotonic() - t0) * 1e6
            cpu     = (u1 - u0) / elapsed * 100.0 if elapsed > 0 else 0.0
            rss, total = _memory(container)
            ifaces = _net_iface_stats(container)
            rx = sum(s["rx_bytes"] for s in ifaces.values())
            tx = sum(s["tx_bytes"] for s in ifaces.values())
            count, avg = _db_stats(name)

            if METRICS_OUT:
                _write_jsonl({
                    "ts":                  time.time(),
                    "node":                name,
                    "cpu_pct":             round(cpu, 3),
                    "mem_rss_bytes":       rss,
                    "mem_total_bytes":     total,
                    "net_rx_bytes":        rx,
                    "net_tx_bytes":        tx,
                    "net_ifaces":          ifaces,
                    "db_stored_packets":   count,
                    "db_avg_delivery_sec": avg,
                })

        except Exception as exc:
            print(f"[exporter] {name}: {exc}", flush=True)

        elapsed_total = time.monotonic() - t0
        stop.wait(max(0.0, INTERVAL - elapsed_total))


def main() -> None:
    print(f"[exporter] interval={INTERVAL}s", flush=True)

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
