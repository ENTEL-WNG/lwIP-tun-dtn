#!/usr/bin/env python3
"""
get_metrics.py — Per-container metrics collector for DTN nodes.

Uses the Docker Stats API (container.stats stream) — single call per
container; daemon reads cgroup directly. Provides CPU, memory, network,
blkio, and pids.

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

import heapq
import json
import os
import platform
import re
import sqlite3
import threading
import time
import tomllib
from datetime import datetime, timezone
from pathlib import Path

import docker

# ── configuration ────────────────────────────────────────────────────────────

NODE_PREFIX      = os.environ.get("NODE_PREFIX",      "node")
PLAN_NAME        = os.environ.get("PLAN_NAME",        "")
TEST_CASE_NUMBER = os.environ.get("TEST_CASE_NUMBER", "0")
DB_DIR           = f"/repo/dtn_storage/{PLAN_NAME}" if PLAN_NAME else ""
METRICS_OUT      = os.environ.get("METRICS_OUT",      "")
CAPTURE_INTERVAL = int(os.environ.get("CAPTURE_INTERVAL", "30"))

# Per-node pcap and netfilter trace captures are expensive (disk + CPU) and
# only useful when debugging. Enable them only at DTN_LOG_LEVEL=DEBUG.
DTN_LOG_LEVEL    = os.environ.get("DTN_LOG_LEVEL", "INFO").upper()
CAPTURE_DEBUG    = DTN_LOG_LEVEL == "DEBUG"

_jsonl_lock = threading.Lock()
_NODE_NAME_MAP: dict[str, str] = {}


def _load_node_name_map() -> dict[str, str]:
    """Map container name (node{id}) → node name from the contact plan."""
    if not PLAN_NAME:
        return {}
    plan_path = f"/repo/networks/{PLAN_NAME}/contact-plan.toml"
    try:
        with open(plan_path, "rb") as f:
            data = tomllib.load(f)
        return {
            f"node{node['id']}": node.get("name") or f"node{node['id']}"
            for node in data.get("nodes", [])
            if node.get("id") is not None
        }
    except Exception as exc:
        print(f"[metrics] could not load contact plan for name map: {exc}", flush=True)
        return {}


# ── shared helpers ────────────────────────────────────────────────────────────

def _db_stats(name: str) -> tuple[int | None, float | None]:
    """Return (count, avg_delivery) from stored_packets, or (None, None)."""
    if not DB_DIR:
        return None, None
    db_path = f"{DB_DIR}/{name}_dtn_packets.db"
    if not os.path.exists(db_path):
        return None, None
    try:
        uri = f"file:{db_path}?mode=ro&nolock=1"
        con = sqlite3.connect(uri, uri=True, timeout=1)
        row = con.execute(
            "SELECT COUNT(*), AVG(min_delivery_time_in_sec) FROM stored_packets"
        ).fetchone()
        con.close()
        count = int(row[0])   if row[0] is not None else 0
        avg   = float(row[1]) if row[1] is not None else None
        return count, avg
    except Exception:
        return None, None


def _write_jsonl(record: dict) -> None:
    try:
        os.makedirs(os.path.dirname(METRICS_OUT), exist_ok=True)
        with _jsonl_lock:
            with open(METRICS_OUT, "a") as f:
                f.write(json.dumps(record) + "\n")
    except Exception as exc:
        print(f"[metrics] jsonl write error: {exc}", flush=True)


# ── stats backend ─────────────────────────────────────────────────────────────

def _poll_stats(container, stop: threading.Event) -> None:
    name = container.name
    print(f"[metrics] polling {name} (stats)", flush=True)
    first = True
    for raw in container.stats(stream=True, decode=True):
        if stop.is_set():
            break
        if first:        # first sample has no preread delta — skip it
            first = False
            continue
        try:
            # CPU %
            cpu_delta = (raw["cpu_stats"]["cpu_usage"]["total_usage"]
                         - raw["precpu_stats"]["cpu_usage"]["total_usage"])
            ncpus = raw["cpu_stats"].get("online_cpus") or len(
                raw["cpu_stats"]["cpu_usage"].get("percpu_usage") or [1])
            sys_cur = raw["cpu_stats"].get("system_cpu_usage")
            sys_pre = raw["precpu_stats"].get("system_cpu_usage")
            if sys_cur is not None and sys_pre is not None:
                sys_delta = sys_cur - sys_pre
                cpu_pct = (cpu_delta / sys_delta) * ncpus * 100.0 if sys_delta > 0 else 0.0
            else:
                # cgroupv2: system_cpu_usage absent — derive from wall-clock timestamps
                read_dt    = datetime.fromisoformat(raw["read"].replace("Z", "+00:00"))
                preread_dt = datetime.fromisoformat(raw["preread"].replace("Z", "+00:00"))
                elapsed_ns = (read_dt - preread_dt).total_seconds() * 1e9
                cpu_pct = (cpu_delta / elapsed_ns) * ncpus * 100.0 if elapsed_ns > 0 else 0.0

            # Memory (usage and limit — directly from Docker Stats)
            mem_s       = raw["memory_stats"]
            mem_usage   = mem_s["usage"]
            mem_limit   = mem_s["limit"]

            # Network (per-interface counters)
            ifaces: dict[str, dict] = {}
            for iface, data in (raw.get("networks") or {}).items():
                ifaces[iface] = {
                    "rx_bytes":   data["rx_bytes"],
                    "rx_packets": data["rx_packets"],
                    "rx_errors":  data["rx_errors"],
                    "rx_drop":    data["rx_dropped"],
                    "tx_bytes":   data["tx_bytes"],
                    "tx_packets": data["tx_packets"],
                    "tx_errors":  data["tx_errors"],
                    "tx_drop":    data["tx_dropped"],
                }

            # Block I/O
            blk_read = blk_write = 0
            for entry in ((raw.get("blkio_stats") or {})
                          .get("io_service_bytes_recursive") or []):
                op = (entry.get("op") or "").lower()
                if op == "read":
                    blk_read += entry["value"]
                elif op == "write":
                    blk_write += entry["value"]

            # PIDs
            pids = (raw.get("pids_stats") or {}).get("current")

            count, avg = _db_stats(_NODE_NAME_MAP.get(name, name))
            if METRICS_OUT:
                _write_jsonl({
                    "ts":                  time.time(),
                    "node":                name,
                    "cpu_pct":             round(cpu_pct, 3),
                    "mem_usage_bytes":     mem_usage,
                    "mem_limit_bytes":     mem_limit,
                    "net_rx_bytes":        sum(s["rx_bytes"] for s in ifaces.values()),
                    "net_tx_bytes":        sum(s["tx_bytes"] for s in ifaces.values()),
                    "net_ifaces":          ifaces,
                    "blk_read_bytes":      blk_read,
                    "blk_write_bytes":     blk_write,
                    "pids":                pids,
                    "db_stored_packets":   count,
                    "db_avg_delivery_sec": avg,
                })
        except Exception as exc:
            print(f"[metrics] {name}: {exc}", flush=True)


# ── per-node packet capture ───────────────────────────────────────────────────

def _start_node_captures(containers: list) -> None:
    """Start per-node tcpdump captures inside each node container.

    Each capture binary is started as its own detached exec so Docker manages
    the process lifetime directly — avoids background-job (`&`) orphan issues
    when the exec shell exits.
    """
    if not PLAN_NAME:
        print("[metrics] PLAN_NAME not set — skipping node captures", flush=True)
        return
    out_dir = f"/repo/networks/{PLAN_NAME}/captures/{TEST_CASE_NUMBER}"
    for c in containers:
        m = re.search(r"\d+", c.name)
        if not m:
            continue
        node_id = m.group()
        base = f"{out_dir}/node{node_id}"
        try:
            # Create output directory and remove stale files from a previous run
            c.exec_run(["sh", "-c",
                        f"mkdir -p {out_dir} && rm -f {base}.pcap {base}.txt {base}_trace.txt"])
            # Human-readable text — needs shell for stdout redirect
            c.exec_run(["sh", "-c",
                        f"tcpdump -i any ip6 -nn -e -l -tttt > {base}.txt 2>&1"],
                       detach=True)
            if CAPTURE_DEBUG:
                # Binary pcap — written directly by tcpdump, no shell needed
                c.exec_run(["tcpdump", "-i", "any", "ip6", "-nn", "-U", "-w", f"{base}.pcap"],
                           detach=True)
                # Netfilter trace
                c.exec_run(["sh", "-c",
                            f"xtables-monitor --trace > {base}_trace.txt 2>&1"],
                           detach=True)
            print(f"[metrics] started capture for {c.name} (run={TEST_CASE_NUMBER}, "
                  f"pcap/trace={'on' if CAPTURE_DEBUG else 'off'})", flush=True)
        except Exception as exc:
            print(f"[metrics] capture start failed for {c.name}: {exc}", flush=True)


# ── log collection ───────────────────────────────────────────────────────────

def _log_file(captures_dir: Path, dt: datetime) -> Path:
    return captures_dir / f"logs_{dt.strftime('%Y-%m-%d_%H')}.txt"


def _collect_once(client, captures_dir: Path, since: datetime | None) -> None:
    lines: list[str] = []
    for c in client.containers.list():
        try:
            raw = c.logs(timestamps=True, since=since, stream=False)
            for line in raw.decode(errors="replace").splitlines():
                if line.strip():
                    lines.append(f"{c.name} | {line}")
        except Exception as exc:
            print(f"[metrics] log collection failed for {c.name}: {exc}", flush=True)
    if not lines:
        return
    lines.sort(key=lambda l: l.split(" | ", 1)[1] if " | " in l else l)
    out = _log_file(captures_dir, datetime.now(timezone.utc))
    with out.open("a") as f:
        f.write("\n".join(lines) + "\n")
    print(f"[metrics] +{len(lines)} log lines → {out.name}", flush=True)


def _merge_tcpdump(captures_dir: Path) -> None:
    files = sorted(captures_dir.glob("node*.txt"))
    if not files:
        return

    def _valid_lines(path: Path):
        with path.open(errors="replace") as fh:
            for line in fh:
                line = line.rstrip("\n")
                if len(line) >= 4 and line[:4].isdigit():
                    yield line

    out = captures_dir / "tcpdump.txt"
    count = 0
    with out.open("w") as fh:
        for line in heapq.merge(*(_valid_lines(p) for p in files)):
            fh.write(line + "\n")
            count += 1
    if count == 0:
        out.unlink(missing_ok=True)
        return
    print(f"[metrics] tcpdump.txt → {out}  ({count} lines)", flush=True)


def _collect_logs_thread(stop: threading.Event, captures_dir: Path) -> None:
    client = docker.from_env(version="auto")
    since: datetime | None = None
    print(f"[metrics] log collection every {CAPTURE_INTERVAL}s → {captures_dir}", flush=True)
    while not stop.wait(timeout=CAPTURE_INTERVAL):
        now = datetime.now(timezone.utc)
        _collect_once(client, captures_dir, since)
        since = now
        _merge_tcpdump(captures_dir)
    _collect_once(client, captures_dir, since)
    _merge_tcpdump(captures_dir)


# ── main ──────────────────────────────────────────────────────────────────────

def main() -> None:
    print(f"[metrics] backend=stats platform={platform.system()} "
          f"log_level={DTN_LOG_LEVEL} pcap/trace={'on' if CAPTURE_DEBUG else 'off'}",
          flush=True)

    if METRICS_OUT:
        os.makedirs(os.path.dirname(os.path.abspath(METRICS_OUT)), exist_ok=True)
        open(METRICS_OUT, "w").close()
        print(f"[metrics] metrics → {METRICS_OUT}", flush=True)

    global _NODE_NAME_MAP
    _NODE_NAME_MAP = _load_node_name_map()
    if _NODE_NAME_MAP:
        print(f"[metrics] node name map: {_NODE_NAME_MAP}", flush=True)

    client = docker.from_env(version="auto")
    stop   = threading.Event()

    containers = [c for c in client.containers.list()
                  if c.name.startswith(NODE_PREFIX)]
    print(f"[metrics] found {len(containers)} node container(s): "
          f"{[c.name for c in containers]}", flush=True)

    _start_node_captures(containers)

    for c in containers:
        threading.Thread(target=_poll_stats, args=(c, stop), daemon=True).start()

    if METRICS_OUT:
        captures_dir = Path(METRICS_OUT).parent
        threading.Thread(
            target=_collect_logs_thread, args=(stop, captures_dir), daemon=True
        ).start()

    try:
        while True:
            time.sleep(60)
    except KeyboardInterrupt:
        stop.set()
        time.sleep(CAPTURE_INTERVAL + 5)  # let log thread finish final collection


if __name__ == "__main__":
    main()
