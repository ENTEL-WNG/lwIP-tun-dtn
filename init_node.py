#!/usr/bin/env python3
"""
init_node.py — Node initialisation script.

Replaces init_dtn_node.sh, init_reg_node.sh, and init_routes.sh.
Configuration is read from a TOML file passed as the first argument
(defaults to "node.toml" in the current directory).

Usage:
    python3 init_node.py [path/to/nodeN.toml]

The script must run as root (or with CAP_NET_ADMIN) inside the container.
"""

from __future__ import annotations

import os
import re
import sys
import time
import subprocess
import tomllib          # stdlib >= 3.11; install 'tomli' for 3.9/3.10
import logging
import ipaddress
from pathlib import Path
from datetime import datetime


# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s.%(msecs)03d [%(levelname)s] %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
log = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def run(cmd: list[str], *, check: bool = True, ignore_errors: bool = False) -> subprocess.CompletedProcess:
    """Run a shell command, logging it first."""
    log.info("$ %s", " ".join(str(c) for c in cmd))
    try:
        result = subprocess.run(cmd, capture_output=True, text=True)
    except FileNotFoundError:
        if ignore_errors:
            log.debug("Command not found, skipping: %s", cmd[0])
            return subprocess.CompletedProcess(cmd, 127, "", "")
        raise
    if result.stdout.strip():
        log.debug("stdout: %s", result.stdout.strip())
    if result.stderr.strip():
        log.debug("stderr: %s", result.stderr.strip())
    if check and not ignore_errors and result.returncode != 0:
        log.error("Command failed (rc=%d): %s", result.returncode, result.stderr.strip())
        raise subprocess.CalledProcessError(result.returncode, cmd)
    return result


def sysctl(key: str, value: str) -> None:
    run(["sysctl", "-w", f"{key}={value}"], ignore_errors=True)


def ip6tables(*args: str, table: str = "filter", ignore_errors: bool = False) -> None:
    run(["ip6tables", "-t", table, *args], ignore_errors=ignore_errors)


def ip(*args: str, ignore_errors: bool = False) -> None:
    run(["ip", *args], ignore_errors=ignore_errors)


# ---------------------------------------------------------------------------
# Config loading
# ---------------------------------------------------------------------------

def load_config(toml_path: str) -> dict:
    path = Path(toml_path)
    if not path.exists():
        log.error("Config file not found: %s", path)
        sys.exit(1)
    with open(path, "rb") as f:
        return tomllib.load(f)


# ---------------------------------------------------------------------------
# Route / interface setup  (replaces init_routes.sh)
# ---------------------------------------------------------------------------

def setup_interfaces(cfg: dict) -> list[str]:
    """
    Rename Docker veth interfaces to the names declared in the TOML,
    assign IPv6 addresses, add routes, and install permanent neighbour
    entries.

    Returns a list of all live interface names (used later by the DTN
    ip6tables rules).
    """
    log.info("--- Setting interfaces and routes ---")
    all_interfaces: list[str] = []
    node_id: int = cfg["node"]["id"]

    for iface in cfg.get("interface", []):
        int_name:    str = iface["name"]
        local_addr:  str = iface["local_addr"].split("/")[0]   # strip prefix len
        local_mac:   str = iface["local_mac"]
        remote_addr: str = iface["remote_addr"].split("/")[0]
        neigh_mac:   str = iface["remote_mac"]

        log.info("=== Configuring %s ===", int_name)

        # ---- Rename the Docker-assigned ethX to our logical name ----------
        result = run(
            ["ip", "-o", "link", "show"],
            check=False,
        )
        old_name: str | None = None
        for line in result.stdout.splitlines():
            if local_mac.lower() in line.lower():
                # Format: "N: ethX@if…: …"
                m = re.search(r":\s+(\S+?)[@:]", line)
                if m:
                    old_name = m.group(1)
                    break

        if old_name and old_name != int_name:
            log.info("Renaming %s -> %s", old_name, int_name)
            tmp = f"tmp_{int_name}"
            ip("link", "set", old_name, "down")
            ip("link", "set", old_name, "name", tmp)
            ip("link", "set", tmp, "name", int_name)
            ip("link", "set", int_name, "up")
        elif not old_name:
            log.warning("Could not find interface with MAC %s; skipping rename.", local_mac)

        # ---- Disable DAD before assigning address (avoid tentative state) --
        sysctl(f"net.ipv6.conf.{int_name}.accept_dad", "0")

        # ---- Assign local IPv6 address -----------------------------------
        ip("-6", "addr", "add", iface["local_addr"], "dev", int_name, ignore_errors=True)

        # ---- Extra routes declared in the interface block ----------------
        #
        # For each address in dtn_addresses / addresses that is NOT on the
        # directly-connected /64, add a /64 prefix route via the remote
        # neighbour.  Host routes (/128) can be silently rejected by the
        # kernel when the gateway hasn't been resolved yet; prefix routes
        # are accepted as soon as the local address is assigned.
        #
        local_net = ipaddress.ip_interface(iface["local_addr"]).network  # e.g. fd00:1:2::/64
        via = remote_addr
        added_nets: set[str] = set()
        for addr_str in iface.get("dtn_addresses", []) + iface.get("addresses", []):
            addr = ipaddress.ip_address(addr_str.split("/")[0])
            if addr in local_net:
                continue  # directly connected, kernel adds this automatically
            net = str(ipaddress.ip_interface(f"{addr}/64").network)
            if net not in added_nets:
                ip("-6", "route", "add", net, "via", via, "dev", int_name,
                   ignore_errors=True)
                added_nets.add(net)

        # ---- Permanent neighbour (ARP/NDP) entry -------------------------
        ip("-6", "neigh", "replace", remote_addr,
           "lladdr", neigh_mac, "dev", int_name, "nud", "permanent",
           ignore_errors=True)

        all_interfaces.append(int_name)

    return all_interfaces


# ---------------------------------------------------------------------------
# Regular-node init  (replaces init_reg_node.sh)
# ---------------------------------------------------------------------------

def init_regular_node(cfg: dict) -> None:
    node_id = cfg["node"]["id"]
    log.info("--- Initializing Regular Node (id=%s) ---", node_id)

    sysctl("net.ipv6.conf.all.accept_dad", "0")
    sysctl("net.ipv6.conf.all.forwarding", "1")

    setup_interfaces(cfg)

    log.info("--- Node %s Setup Complete. Keeping container alive ---", node_id)
    # Equivalent of `exec sleep infinity`
    while True:
        time.sleep(3600)


# ---------------------------------------------------------------------------
# DTN-node init  (replaces init_dtn_node.sh)
# ---------------------------------------------------------------------------

def init_dtn_node(cfg: dict) -> None:
    node_id       = cfg["node"]["id"]
    tun_addr      = cfg["node"]["tun_ipv6_addr"].split("/")[0]
    lwip_addr     = cfg["node"]["lwip_ipv6_addr"].split("/")[0]

    log.info("--- Initializing DTN Node (id=%s) ---", node_id)

    # ---- 1. Kernel settings -----------------------------------------------
    sysctl("net.ipv6.conf.all.accept_dad", "0")
    sysctl("net.ipv6.conf.all.forwarding", "1")

    # ---- 2. Interfaces & routes -------------------------------------------
    all_interfaces = setup_interfaces(cfg)

    # ---- 3. TUN interface -------------------------------------------------
    log.info("Setting up /dev/net/tun...")
    Path("/dev/net").mkdir(parents=True, exist_ok=True)
    tun_dev = Path("/dev/net/tun")
    if not tun_dev.is_char_device():
        run(["mknod", "/dev/net/tun", "c", "10", "200"])

    ip("tuntap", "add", "dev", "tun0", "mode", "tun", ignore_errors=True)
    ip("-6", "addr", "add", f"{tun_addr}/64", "dev", "tun0", ignore_errors=True)
    ip("link", "set", "tun0", "up")

    # ---- 4. ip6tables — flush existing rules ------------------------------
    log.info("Flushing existing ip6tables rules...")
    for table, chain in [
        ("raw",    "PREROUTING"),
        ("raw",    "OUTPUT"),
        ("mangle", "PREROUTING"),
        ("mangle", "OUTPUT"),
        ("filter", "OUTPUT"),
    ]:
        ip6tables("-F", chain, table=table, ignore_errors=True)

    # ---- 5. Optional: netfilter logging (best-effort) ---------------------
    run(["modprobe", "nf_log_ipv6"], ignore_errors=True)
    sysctl("net.netfilter.nf_log.10", "nf_log_ipv6")

    # ---- 6. ip6tables — TRACE (debug) ------------------------------------
    for iface in all_interfaces:
        ip6tables("-A", "PREROUTING", "-i", iface, "-j", "TRACE",
                  table="raw", ignore_errors=True)
    ip6tables("-A", "OUTPUT", "-j", "TRACE", table="raw", ignore_errors=True)

    # ---- 7. Neighbour Discovery exemptions --------------------------------
    # for nd_type in (133, 134, 135, 136):
    #     ip6tables("-A", "PREROUTING",
    #               "-p", "icmpv6", "--icmpv6-type", str(nd_type), "-j", "ACCEPT",
    #               table="mangle")

    # ---- 8. Mark all ingress traffic on data interfaces ------------------
    for iface in all_interfaces:
        log.info("Marking PREROUTING on %s -> mark 1", iface)
        ip6tables("-A", "PREROUTING", "-i", iface, "-j", "MARK", "--set-mark", "1",
                  table="mangle")

    # Exempt raw-socket egress (fwmark 2) from re-marking with 1.
    # Without this, AF_INET6 raw socket packets would be marked 1 by the next
    # rule, hit table 100, and loop back through tun0.
    ip6tables("-A", "OUTPUT", "-m", "mark", "--mark", "2", "-j", "RETURN", table="mangle")
    ip6tables("-A", "OUTPUT", "-j", "MARK", "--set-mark", "1", table="mangle")

    # ---- 9. Docker fix: demote 'local' table to prio 1000 ---------------
    # Must happen before we add our fwmark rule at prio 0, otherwise
    # `ip rule del pref 0` would remove the fwmark rule instead of local.
    result = run(["ip", "-6", "rule", "show"], check=False)
    if re.search(r"^0:.*lookup local", result.stdout, re.MULTILINE):
        ip("-6", "rule", "add", "pref", "1000", "table", "local",
           ignore_errors=True)
        ip("-6", "rule", "del", "pref", "0", ignore_errors=True)

    # ---- 10. Policy routing: fwmark 1 -> table 100 -> via LwIP ----------
    ip("-6", "rule", "del", "fwmark", "1", "table", "100", ignore_errors=True)
    ip("-6", "rule", "add", "fwmark", "1", "table", "100", "priority", "0")
    ip("-6", "route", "replace", "default",
       "via", lwip_addr, "dev", "tun0", "table", "100")

    # ---- 10b. Policy routing: fwmark 2 -> table 200 -> physical egress ---
    # Raw sockets (AF_INET6) in lwip_tun are marked 2. Without a dedicated
    # table they would fall through to the main table where routes may not
    # exist, or worse hit table 100 and loop back through tun0.
    ip("-6", "rule", "del", "fwmark", "2", "table", "200", ignore_errors=True)
    ip("-6", "rule", "add", "fwmark", "2", "table", "200", "priority", "1")
    for iface in cfg.get("interface", []):
        int_name    = iface["name"]
        local_net   = ipaddress.ip_interface(iface["local_addr"]).network
        remote_addr = iface["remote_addr"].split("/")[0]
        # Directly-connected /64
        ip("-6", "route", "replace", str(local_net), "dev", int_name, "table", "200",
           ignore_errors=True)
        # Non-local reachable prefixes — one /64 per listed DTN/regular address
        added_nets_200: set[str] = set()
        for addr_str in iface.get("dtn_addresses", []) + iface.get("addresses", []):
            addr = ipaddress.ip_address(addr_str.split("/")[0])
            if addr in local_net:
                continue
            net = str(ipaddress.ip_interface(f"{addr}/64").network)
            if net not in added_nets_200:
                ip("-6", "route", "replace", net, "via", remote_addr, "dev", int_name,
                   "table", "200", ignore_errors=True)
                added_nets_200.add(net)

    # ---- 11. Hand off to LwIP binary -------------------------------------
    log.info("--- Setup Complete. Starting LwIP binary ---")
    time.sleep(2)
    lwip_bin = Path("./lwip_tun")
    if not lwip_bin.exists():
        log.error("lwip_tun binary not found at %s", lwip_bin.resolve())
        sys.exit(1)

    # os.environ["DTN_CONFIG_PATH"] = toml_path
    os.execv(str(lwip_bin), [str(lwip_bin)])   # replaces this process
    log.info("--- Node %s Setup Complete. Keeping container alive ---", node_id)
    # Equivalent of `exec sleep infinity`
    while True:
        time.sleep(3600)

# ---------------------------------------------------------------------------
# Build binary
# ---------------------------------------------------------------------------

def build_binary() -> None:
         log.info("--- Building lwip_tun ---")
         result = run(["make", "-C", "/repo"], check=False)
         if result.returncode != 0:
             log.error("make failed (rc=%d):\n%s", result.returncode, result.stderr.strip())
             sys.exit(1)
         log.info("--- Build complete ---")

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    toml_path = sys.argv[1] if len(sys.argv) > 1 else "node.toml"
    cfg = load_config(toml_path)
    build_binary()

    node = cfg.get("node", {})
    is_dtn: bool = node.get("is_dtn", False)

    log.info(
        "=== Node %s (%s) — %s ===",
        node.get("id", "?"),
        node.get("name", "unnamed"),
        "DTN" if is_dtn else "Regular",
    )

    if is_dtn:
        init_dtn_node(cfg)
    else:
        init_regular_node(cfg)


if __name__ == "__main__":
    main()