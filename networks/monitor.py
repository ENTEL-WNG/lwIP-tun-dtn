#!/usr/bin/env python3
"""
monitor.py — CPU / memory / network / SQLite DB monitor for DTN experiments.

Uses `docker exec` to read cgroup v2 and /proc files from inside each
container.  This approach works on Docker Desktop for macOS (and Linux),
where the Docker Engine API /containers/{id}/stats endpoint returns empty
or all-zero data because the daemon's cgroup collector is not wired through
to the macOS host.

Each background thread per container runs a tiny inline Python script via
`docker exec python3 -c ...` that:
  - reads /sys/fs/cgroup/cpu.stat (usage_usec) twice, 1 s apart → CPU %
  - reads /sys/fs/cgroup/memory.current and memory.stat → RSS in MiB
  - reads /proc/net/dev → cumulative RX/TX bytes (excluding lo)

All threads run in parallel so the main loop never blocks longer than one
sampling cycle regardless of container count.

One JSON object per container per tick (JSONL):
  {
    "ts":          float,  # seconds since epoch
    "node":        str,    # container name
    "cpu_pct":     float,  # CPU % (0–100 × ncores);           -1 on error
    "mem_mb":      float,  # RSS memory in MiB (excl. cache);  -1 on error
    "net_rx_kb":   float,  # cumulative RX since start, KiB;   -1 on error
    "net_tx_kb":   float,  # cumulative TX since start, KiB;   -1 on error
    "db_count":    int,    # stored_packets row count;          -1 if no DB
    "db_avg_del":  float   # AVG(delivery_time_in_sec);         -1 if unavail.
  }

Usage:
  python3 monitor.py --containers node1,node2,node3 \\
                     --db-map node2=/path/to/node2_dtn_packets.db \\
                     --interval 1 \\
                     --out captures/latest/metrics.jsonl
"""

import argparse
import json
import os
import signal
import sqlite3
import subprocess
import sys
import threading
import time

# ---------------------------------------------------------------------------
# Inline script executed inside each container via `docker exec python3 -c`
# ---------------------------------------------------------------------------

_CONTAINER_STATS_SCRIPT = r"""
import json, time

def _read(path, default=''):
    try:
        return open(path).read().strip()
    except Exception:
        return default

# --- Memory (cgroup v2) ---
try:
    current   = int(_read('/sys/fs/cgroup/memory.current', '0') or '0')
    mem_stats = {}
    for _line in _read('/sys/fs/cgroup/memory.stat', '').splitlines():
        _p = _line.split()
        if len(_p) == 2:
            try:
                mem_stats[_p[0]] = int(_p[1])
            except ValueError:
                pass
    # 'file' covers page cache; 'anon' is pure RSS
    file_cache = mem_stats.get('file', 0)
    mem_mb     = (current - file_cache) / (1024 ** 2)
except Exception:
    mem_mb = -1.0

# --- CPU (cgroup v2 cpu.stat delta over 1 second) ---
def _get_usage_usec():
    for _line in _read('/sys/fs/cgroup/cpu.stat', '').splitlines():
        if _line.startswith('usage_usec'):
            try:
                return int(_line.split()[1])
            except (IndexError, ValueError):
                return 0
    return 0

try:
    _t0 = time.monotonic();  _u0 = _get_usage_usec()
    time.sleep(1.0)
    _t1 = time.monotonic();  _u1 = _get_usage_usec()
    _elapsed_us = (_t1 - _t0) * 1e6
    cpu_pct = (_u1 - _u0) / _elapsed_us * 100.0 if _elapsed_us > 0 else 0.0
except Exception:
    cpu_pct = -1.0

# --- Network (/proc/net/dev cumulative counters) ---
try:
    _rx_b = _tx_b = 0
    for _line in _read('/proc/net/dev', '').splitlines()[2:]:
        _parts = _line.split()
        if not _parts:
            continue
        _iface = _parts[0].rstrip(':')
        if _iface == 'lo':
            continue
        _rx_b += int(_parts[1])
        _tx_b += int(_parts[9])
    net_rx_kb = _rx_b / 1024
    net_tx_kb = _tx_b / 1024
except Exception:
    net_rx_kb = net_tx_kb = -1.0

print(json.dumps({
    'cpu_pct':    round(cpu_pct,    2),
    'mem_mb':     round(mem_mb,     2),
    'net_rx_kb':  round(net_rx_kb,  2),
    'net_tx_kb':  round(net_tx_kb,  2),
}))
"""


def _container_stats(container: str) -> dict:
    """
    Run a short inline Python script inside the container to collect
    cgroup v2 CPU, memory, and network stats.

    The script sleeps 1 second internally to measure CPU delta, so this
    call blocks for approximately 1 second.
    """
    try:
        result = subprocess.run(
            ["docker", "exec", container, "python3", "-c", _CONTAINER_STATS_SCRIPT],
            capture_output=True,
            text=True,
            timeout=10.0,
        )
        if result.returncode != 0 or not result.stdout.strip():
            raise RuntimeError(
                f"exit {result.returncode}: {result.stderr.strip()[:200]}"
            )
        return json.loads(result.stdout.strip())
    except Exception as e:
        print(
            f"[monitor] stats error for {container}: {e}", file=sys.stderr, flush=True
        )
        return {
            "cpu_pct": -1.0,
            "mem_mb": -1.0,
            "net_rx_kb": -1.0,
            "net_tx_kb": -1.0,
        }


# ---------------------------------------------------------------------------
# Per-container polling threads
# ---------------------------------------------------------------------------

_stats_lock = threading.Lock()
_latest: dict = {}  # {container: {cpu_pct, mem_mb, net_rx_kb, net_tx_kb}}


def _poll_thread(
    container: str, stop_evt: threading.Event, interval: float
) -> None:
    """Collect stats from inside the container every `interval` seconds."""
    while not stop_evt.is_set():
        t0    = time.monotonic()
        entry = _container_stats(container)
        with _stats_lock:
            _latest[container] = entry
        # The exec call already took ~1 s (CPU sleep); only sleep extra if
        # the whole thing finished faster than the requested interval.
        elapsed = time.monotonic() - t0
        stop_evt.wait(timeout=max(0.0, interval - elapsed))


def _start_poll_threads(
    containers: list, stop_evt: threading.Event, interval: float
) -> list:
    threads = []
    for c in containers:
        t = threading.Thread(
            target=_poll_thread,
            args=(c, stop_evt, interval),
            daemon=True,
        )
        t.start()
        threads.append(t)
    return threads


def _read_latest(containers: list) -> dict:
    empty = {
        "cpu_pct": -1.0,
        "mem_mb": -1.0,
        "net_rx_kb": -1.0,
        "net_tx_kb": -1.0,
    }
    with _stats_lock:
        return {c: dict(_latest.get(c, empty)) for c in containers}


# ---------------------------------------------------------------------------
# SQLite DB stats
# ---------------------------------------------------------------------------


def db_stats(db_path: str) -> dict:
    if not db_path or not os.path.exists(db_path):
        return {"db_count": -1, "db_avg_del": -1.0}
    try:
        uri = f"file:{db_path}?mode=ro&nolock=1"
        con = sqlite3.connect(uri, uri=True, timeout=1)
        row = con.execute(
            "SELECT COUNT(*), AVG(delivery_time_in_sec) FROM stored_packets"
        ).fetchone()
        con.close()
        return {
            "db_count":   int(row[0])   if row[0] is not None else 0,
            "db_avg_del": float(row[1]) if row[1] is not None else -1.0,
        }
    except Exception as e:
        return {"db_count": -1, "db_avg_del": -1.0, "_db_err": str(e)}


# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------


def parse_args():
    p = argparse.ArgumentParser(description="DTN experiment resource monitor")
    p.add_argument(
        "--containers", required=True, help="Comma-separated list of container names"
    )
    p.add_argument(
        "--db-map",
        action="append",
        default=[],
        metavar="NODE=PATH",
        help=(
            "Map a container to its SQLite DB. Repeat for multiple nodes. "
            "Example: --db-map node2=/path/node2_dtn_packets.db"
        ),
    )
    p.add_argument(
        "--interval",
        type=float,
        default=1.0,
        help="Sampling interval in seconds (default: 1.0)",
    )
    p.add_argument(
        "--out",
        default="/tmp/metrics.jsonl",
        help="Output JSONL path (default: /tmp/metrics.jsonl)",
    )
    return p.parse_args()


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

_running = True


def _shutdown(signum, frame):
    global _running
    print(f"\n[monitor] signal {signum} — stopping", flush=True)
    _running = False


def main():
    args       = parse_args()
    containers = [c.strip() for c in args.containers.split(",") if c.strip()]

    # DB map
    container_db: dict = {c: "" for c in containers}
    for entry in args.db_map:
        if "=" not in entry:
            print(
                f"[monitor] WARNING: ignoring --db-map {entry!r} (expected NODE=PATH)",
                file=sys.stderr,
                flush=True,
            )
            continue
        node, path = entry.split("=", 1)
        node, path = node.strip(), path.strip()
        if node in container_db:
            container_db[node] = path
        else:
            print(
                f"[monitor] WARNING: --db-map node {node!r} not in --containers",
                file=sys.stderr,
                flush=True,
            )

    signal.signal(signal.SIGTERM, _shutdown)
    signal.signal(signal.SIGINT, _shutdown)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)

    print(f"[monitor] containers  : {containers}", flush=True)
    print(f"[monitor] db map      : {container_db}", flush=True)
    print(f"[monitor] interval    : {args.interval}s", flush=True)
    print(f"[monitor] output      : {args.out}", flush=True)

    # Start per-container polling threads
    stop_evt = threading.Event()
    _start_poll_threads(containers, stop_evt, args.interval)

    # Give threads one full cycle (the exec takes ~1 s for the CPU sample)
    print(f"[monitor] waiting {args.interval + 1:.0f}s for first samples…", flush=True)
    time.sleep(args.interval + 1)

    with open(args.out, "w") as out_f:
        while _running:
            tick_start = time.time()

            stats = _read_latest(containers)
            for container in containers:
                record: dict = {"ts": tick_start, "node": container}
                record.update(stats[container])
                record.update(db_stats(container_db.get(container, "")))
                out_f.write(json.dumps(record) + "\n")

            out_f.flush()

            elapsed   = time.time() - tick_start
            sleep_for = max(0.0, args.interval - elapsed)
            if sleep_for > 0:
                time.sleep(sleep_for)

    stop_evt.set()
    print("[monitor] done", flush=True)


if __name__ == "__main__":
    main()
