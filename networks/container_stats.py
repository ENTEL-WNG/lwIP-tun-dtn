#!/usr/bin/env python3
"""
container_stats.py — Collect one cgroup v2 / procfs stats sample from inside a container.

Reads:
  /sys/fs/cgroup/memory.current + memory.stat  → RSS in MiB
  /sys/fs/cgroup/cpu.stat (delta over 1 s)     → CPU %
  /proc/net/dev                                 → cumulative RX/TX KiB (excl. lo)

Sleeps 1 second internally for the CPU delta measurement, then prints one JSON
object to stdout:
  {"cpu_pct": float, "mem_mb": float, "net_rx_kb": float, "net_tx_kb": float}

All values are -1.0 on error.
"""

import json
import time


def _read(path, default=""):
    try:
        return open(path).read().strip()
    except Exception:
        return default


# --- Memory (cgroup v2) ---
try:
    current = int(_read("/sys/fs/cgroup/memory.current", "0") or "0")
    mem_stats = {}
    for _line in _read("/sys/fs/cgroup/memory.stat", "").splitlines():
        _p = _line.split()
        if len(_p) == 2:
            try:
                mem_stats[_p[0]] = int(_p[1])
            except ValueError:
                pass
    file_cache = mem_stats.get("file", 0)
    mem_mb = (current - file_cache) / (1024**2)
except Exception:
    mem_mb = -1.0


# --- CPU (cgroup v2 cpu.stat delta over 1 second) ---
def _get_usage_usec():
    for _line in _read("/sys/fs/cgroup/cpu.stat", "").splitlines():
        if _line.startswith("usage_usec"):
            try:
                return int(_line.split()[1])
            except (IndexError, ValueError):
                return 0
    return 0


try:
    _t0 = time.monotonic()
    _u0 = _get_usage_usec()
    time.sleep(1.0)
    _t1 = time.monotonic()
    _u1 = _get_usage_usec()
    _elapsed_us = (_t1 - _t0) * 1e6
    cpu_pct = (_u1 - _u0) / _elapsed_us * 100.0 if _elapsed_us > 0 else 0.0
except Exception:
    cpu_pct = -1.0


# --- Network (/proc/net/dev cumulative counters) ---
try:
    _rx_b = _tx_b = 0
    for _line in _read("/proc/net/dev", "").splitlines()[2:]:
        _parts = _line.split()
        if not _parts:
            continue
        _iface = _parts[0].rstrip(":")
        if _iface == "lo":
            continue
        _rx_b += int(_parts[1])
        _tx_b += int(_parts[9])
    net_rx_kb = _rx_b / 1024
    net_tx_kb = _tx_b / 1024
except Exception:
    net_rx_kb = net_tx_kb = -1.0


print(
    json.dumps(
        {
            "cpu_pct": round(cpu_pct, 2),
            "mem_mb": round(mem_mb, 2),
            "net_rx_kb": round(net_rx_kb, 2),
            "net_tx_kb": round(net_tx_kb, 2),
        }
    )
)
