#!/usr/bin/env python3
"""
throughput_test.py — End-to-end DTN throughput experiment.

Steps:
  1.  Generate per-node configs + docker-compose from the contact plan
  2.  docker compose up -d --build
  3.  Wait for all containers to be running
  4.  Start monitor.py on the host (background)
  5.  Start traffic_recv.py inside the receiver container (background)
  6.  Sleep 1 s to let the receiver bind
  7.  Run traffic_gen.py inside the sender container (foreground; blocks until done)
  8.  Wait --wait-after seconds for in-flight / stored packets to drain
  9.  Collect docker compose logs → captures/logs.txt
  10. docker cp sent.csv and recv.csv from containers
  11. Stop monitor.py
  12. docker compose down
  13. Merge per-node tcpdump files → captures/tcpdump.txt
  14. Run analyze.py → captures/report.{txt,json,html}

Usage:
  python3 throughput_test.py [options]

Options:
  --contact-plan FILE   Contact plan TOML (default: networks/contact-plan-throughput.toml)
  --rate N              Packets/second sent by traffic_gen (default: 100)
  --duration N          Sender duration in seconds (default: 30)
  --size N              Payload size in bytes (default: 512)
  --port N              UDP port (default: 5005)
  --sender NODE         Sender container name (auto-detected: first non-DTN node)
  --receiver NODE       Receiver container name (auto-detected: last non-DTN node)
  --relay NODES         Comma-separated DTN relay names (auto-detected: all DTN nodes)
  --dst-addr ADDR       IPv6 destination address for traffic_gen (auto-detected from receiver)
  --wait-after N        Seconds to wait after sender finishes (default: 15)
  --no-analyze          Skip the analyze.py step
  --keep-up             Do not call docker compose down at the end

Example:
  python3 throughput_test.py \\
    --rate 100        \\
    --duration 60     \\
    --size 1024       \\
    --wait-after 30   \\
    --contact-plan networks/contact-plan-udp-cgr.toml
"""

import argparse
import re
import shlex
import subprocess
import sys
import time
import tomllib
from pathlib import Path

SCRIPT_DIR = Path(__file__).parent.resolve()


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def run(cmd: list, **kwargs) -> subprocess.CompletedProcess:
    print(f"  $ {shlex.join(str(c) for c in cmd)}")
    return subprocess.run(cmd, check=True, **kwargs)


def node_roles(contact_plan: Path) -> dict:
    """Parse the contact plan and return auto-detected node roles."""
    with open(contact_plan, "rb") as f:
        data = tomllib.load(f)

    nodes = data.get("nodes", [])
    edges = data.get("edges", [])

    non_dtn = sorted(n["id"] for n in nodes if not n.get("isDtnNode", False))
    dtn     = sorted(n["id"] for n in nodes if n.get("isDtnNode", False))
    all_ids = sorted(n["id"] for n in nodes)

    sender_id = non_dtn[0]  if non_dtn  else all_ids[0]
    recv_id   = non_dtn[-1] if non_dtn  else all_ids[-1]

    recv_edges = [(e["from"], e["to"]) for e in edges
                  if e["from"] == recv_id or e["to"] == recv_id]
    if recv_edges:
        a, b = recv_edges[0]
        lo, hi = min(a, b), max(a, b)
        dst = f"fd00:{lo:02x}:{hi:02x}::{recv_id:x}"
    else:
        dst = ""

    return {
        "sender":   f"node{sender_id}",
        "receiver": f"node{recv_id}",
        "relays":   [f"node{n}" for n in dtn],
        "all":      [f"node{n}" for n in all_ids],
        "dst_addr": dst,
    }


def wait_containers(compose_file: Path, timeout: int = 90) -> None:
    """Poll until all containers in the compose file are running."""
    elapsed = 0
    while True:
        total = subprocess.run(
            ["docker", "compose", "-f", str(compose_file), "ps", "-q"],
            capture_output=True, text=True,
        ).stdout.strip().splitlines()
        running = subprocess.run(
            ["docker", "compose", "-f", str(compose_file), "ps", "--status", "running", "-q"],
            capture_output=True, text=True,
        ).stdout.strip().splitlines()
        if total and len(running) == len(total):
            print(f"All {len(total)} container(s) running.")
            return
        if elapsed >= timeout:
            raise TimeoutError(
                f"Only {len(running)}/{len(total)} container(s) running after {timeout}s"
            )
        time.sleep(2)
        elapsed += 2


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    p = argparse.ArgumentParser(
        description="End-to-end DTN throughput experiment",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--contact-plan",
                   default=str(SCRIPT_DIR / "networks/contact-plan-throughput.toml"))
    p.add_argument("--rate",       type=int, default=100)
    p.add_argument("--duration",   type=int, default=30)
    p.add_argument("--size",       type=int, default=512)
    p.add_argument("--port",       type=int, default=5005)
    p.add_argument("--sender",     default="", metavar="NODE")
    p.add_argument("--receiver",   default="", metavar="NODE")
    p.add_argument("--relay",      default="", metavar="NODES", help="comma-separated relay node names")
    p.add_argument("--dst-addr",   default="", metavar="ADDR")
    p.add_argument("--wait-after", type=int, default=15)
    p.add_argument("--no-analyze", action="store_true")
    p.add_argument("--keep-up",    action="store_true")
    args = p.parse_args()

    contact_plan = Path(args.contact_plan)
    if not contact_plan.is_file():
        sys.exit(f"ERROR: Contact plan not found: {contact_plan}")

    print()
    print(f"=== Throughput test: {contact_plan} ===")
    print(f"    rate={args.rate} pkt/s  duration={args.duration}s"
          f"  size={args.size}B  port={args.port}")
    print()

    # -------------------------------------------------------------------------
    # Step 1 — Generate network configs
    # -------------------------------------------------------------------------
    print("--- [1/8] Generating network configs ---")
    gen = subprocess.run(
        [sys.executable, str(SCRIPT_DIR / "generate-network.py"), str(contact_plan)],
        stdout=subprocess.PIPE, text=True, cwd=str(SCRIPT_DIR), check=True,
    )
    print(gen.stdout, end="")

    compose_dir = next(
        (Path(line.split("=", 1)[1]) for line in gen.stdout.splitlines()
         if line.startswith("OUTPUT_DIR=")),
        None,
    )
    if compose_dir is None:
        sys.exit("ERROR: generate-network.py did not print OUTPUT_DIR=...")

    compose_file = compose_dir / "docker-compose.yml"
    if not compose_file.is_file():
        sys.exit(f"ERROR: docker-compose.yml not found: {compose_file}")

    env_file = compose_dir / ".env"
    test_case_number = 0
    if env_file.is_file():
        for line in env_file.read_text().splitlines():
            if line.startswith("TEST_CASE_NUMBER="):
                try:
                    test_case_number = int(line.split("=", 1)[1])
                except ValueError:
                    pass

    captures_dir = compose_dir / "captures" / str(test_case_number)
    captures_dir.mkdir(parents=True, exist_ok=True)
    plan_name = compose_dir.name

    # -------------------------------------------------------------------------
    # Auto-detect node roles; CLI args take precedence
    # -------------------------------------------------------------------------
    roles = node_roles(contact_plan)
    sender_node = args.sender   or roles["sender"]
    recv_node   = args.receiver or roles["receiver"]
    relay_nodes = (
        [r.strip() for r in args.relay.split(",") if r.strip()]
        if args.relay else roles["relays"]
    )
    dst_addr   = args.dst_addr or roles["dst_addr"]
    all_nodes  = roles["all"]
    containers = ",".join(all_nodes)

    db_map_args: list[str] = []
    for relay in relay_nodes:
        db_path = SCRIPT_DIR / "dtn_storage" / plan_name / f"{relay}_dtn_packets.db"
        db_map_args += ["--db-map", f"{relay}={db_path}"]

    print()
    print(f"    Sender      : {sender_node}")
    print(f"    Receiver    : {recv_node}  (dst [{dst_addr}]:{args.port})")
    print(f"    Relays      : {' '.join(relay_nodes)}")
    print(f"    All nodes   : {' '.join(all_nodes)}")
    print(f"    Compose dir : {compose_dir}")
    print(f"    Captures dir: {captures_dir}")

    # -------------------------------------------------------------------------
    # Idempotent teardown helpers
    # -------------------------------------------------------------------------
    monitor_proc: subprocess.Popen | None = None
    _monitor_done = False
    _compose_down = False

    def stop_monitor() -> None:
        nonlocal _monitor_done
        if _monitor_done:
            return
        _monitor_done = True
        if monitor_proc is not None and monitor_proc.poll() is None:
            print(f"Stopping monitor (pid {monitor_proc.pid})...")
            monitor_proc.terminate()
            try:
                monitor_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                monitor_proc.kill()

    def stop_receiver() -> None:
        subprocess.run(
            ["docker", "exec", recv_node, "sh", "-c",
             "kill $(pgrep -f traffic_recv.py) 2>/dev/null || true"],
            capture_output=True,
        )

    def do_compose_down() -> None:
        nonlocal _compose_down
        if _compose_down or args.keep_up:
            return
        _compose_down = True
        print("docker compose down...")
        subprocess.run(
            ["docker", "compose", "-f", str(compose_file), "down"],
            check=False,
        )

    try:
        # -------------------------------------------------------------------------
        # Step 2 — docker compose up
        # -------------------------------------------------------------------------
        print()
        print("--- [2/8] Starting docker compose ---")
        run(["docker", "compose", "-f", str(compose_file), "up", "-d", "--build"])

        # -------------------------------------------------------------------------
        # Step 3 — Wait for containers
        # -------------------------------------------------------------------------
        print("--- [3/8] Waiting for containers to start ---")
        wait_containers(compose_file)
        time.sleep(3)  # let lwip_tun finish initialising

        # -------------------------------------------------------------------------
        # Step 4 — Start monitor.py (background)
        # -------------------------------------------------------------------------
        print()
        print("--- [4/8] Starting monitor.py (host, background) ---")
        monitor_cmd = [
            sys.executable, str(SCRIPT_DIR / "networks/monitor.py"),
            "--containers", containers,
            *db_map_args,
            "--interval", "1",
            "--out", str(captures_dir / "metrics.jsonl"),
        ]
        print(f"  $ {shlex.join(str(c) for c in monitor_cmd)}")
        monitor_proc = subprocess.Popen(monitor_cmd)
        print(f"monitor.py pid: {monitor_proc.pid}")

        # -------------------------------------------------------------------------
        # Step 5 — Start receiver (background)
        # -------------------------------------------------------------------------
        print()
        print(f"--- [5/8] Starting traffic_recv.py in {recv_node} ---")
        run(["docker", "exec", "-d", recv_node,                                                                                                                           
            "python3", "/repo/networks/traffic_recv.py", str(args.port),                                                                                          
            "--out", "/tmp/recv.csv"])      
        time.sleep(1)

        # -------------------------------------------------------------------------
        # Step 6 — Run sender (foreground, blocks until done)
        # -------------------------------------------------------------------------
        print()
        print(f"--- [6/8] Running traffic_gen.py in {sender_node} ---")
        print(f"    {sender_node} -> [{dst_addr}]:{args.port}"
              f"  rate={args.rate} pkt/s  {args.duration}s  {args.size}B")
        run(["docker", "exec", sender_node,
             "python3", "/repo/networks/traffic_gen.py", dst_addr, str(args.port),
             "--rate",     str(args.rate),
             "--duration", str(args.duration),
             "--size",     str(args.size),
             "--out",      "/tmp/sent.csv"])
        print("Sender finished.")

        # -------------------------------------------------------------------------
        # Step 7 — Wait for drain
        # -------------------------------------------------------------------------
        print()
        print(f"--- [7/8] Waiting {args.wait_after}s for in-flight packets to drain ---")
        time.sleep(args.wait_after)

        # -------------------------------------------------------------------------
        # Step 8 — Collect artefacts
        # -------------------------------------------------------------------------
        print()
        print("--- [8/8] Collecting artefacts ---")

        print("Stopping receiver...")
        stop_receiver()
        time.sleep(1)

        for container, src, name in [
            (sender_node, "/tmp/sent.csv", "sent.csv"),
            (recv_node,   "/tmp/recv.csv", "recv.csv"),
        ]:
            print(f"Copying {name} from {container}...")
            r = subprocess.run(
                ["docker", "cp", f"{container}:{src}", str(captures_dir / name)],
                capture_output=True,
            )
            if r.returncode != 0:
                print(f"WARNING: {name} not found in {container}")

        print("Collecting docker compose logs...")
        log_result = subprocess.run(
            ["docker", "compose", "-f", str(compose_file),
             "logs", "--no-color", "--timestamps"],
            capture_output=True, text=True,
        )
        log_lines = [
            re.sub(r"(\S+)\s*\|", r"\1 |", line, count=1)
            for line in (log_result.stdout + log_result.stderr).splitlines()
        ]
        log_lines.sort(key=lambda l: l.split()[2] if len(l.split()) >= 3 else l)
        (captures_dir / "logs.txt").write_text("\n".join(log_lines) + "\n")
        print(f"Logs → {captures_dir}/logs.txt")

        stop_monitor()
        print(f"Metrics → {captures_dir}/metrics.jsonl")

        do_compose_down()

        print("Merging per-node tcpdump files...")
        tcpdump_lines: list[str] = []
        for txt in sorted(captures_dir.glob("node*.txt")):
            for line in txt.read_text(errors="replace").splitlines():
                if len(line) >= 4 and line[:4].isdigit():
                    tcpdump_lines.append(line)
        if tcpdump_lines:
            tcpdump_lines.sort()
            (captures_dir / "tcpdump.txt").write_text("\n".join(tcpdump_lines) + "\n")
            print(f"Merged tcpdump → {captures_dir}/tcpdump.txt")
        else:
            print(f"WARNING: no node*.txt capture files found in {captures_dir}")

        if not args.no_analyze:
            print()
            print("--- [analyze] Running analyze.py ---")
            subprocess.run(
                [sys.executable, str(SCRIPT_DIR / "networks/analyze.py"), str(captures_dir)],
                check=False,
            )

        print()
        print("=" * 60)
        print("  Throughput test complete")
        print("=" * 60)
        print(f"  Contact plan : {contact_plan.name}")
        print(f"  Run index    : {test_case_number}")
        print(f"  Sender       : {sender_node}  ->  [{dst_addr}]:{args.port}")
        print(f"  Receiver     : {recv_node}")
        print(f"  Relays       : {' '.join(relay_nodes)}")
        print(f"  Captures dir : {captures_dir}")
        print()
        print("  Files:")
        print("    sent.csv      — per-packet send timestamps (sender)")
        print("    recv.csv      — per-packet receive timestamps (receiver)")
        print("    metrics.jsonl — per-second CPU / mem / DB snapshots")
        print("    logs.txt      — docker compose logs (all nodes)")
        print("    tcpdump.txt   — merged per-node traffic (time-sorted)")
        print("    node*.pcap    — per-node binary captures")
        print("    report.txt    — PDR / latency / throughput summary")
        print("    report.json   — machine-readable report")
        print("    report.html   — charts (if matplotlib is installed)")
        print("=" * 60)

    finally:
        print()
        print("--- [cleanup] ---")
        stop_monitor()
        stop_receiver()
        do_compose_down()


if __name__ == "__main__":
    main()
