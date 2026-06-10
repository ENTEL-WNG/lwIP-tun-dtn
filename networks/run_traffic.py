#!/usr/bin/env python3
"""
run_traffic.py — Traffic generation/collection for a DTN throughput experiment.

Assumes the docker compose stack is already running:
    cd <network_dir> && docker compose up -d --build

This script only:
  1. Starts traffic_recv.py inside the receiver container (background)
  2. Runs  traffic_gen.py  inside the sender container (foreground)
  3. Waits --wait-after seconds for in-flight packets to drain
  4. Collects artefacts: sent.csv, recv.csv, logs.txt, tcpdump.txt, report.*

Usage:
    python3 run_traffic.py [options]

Options:
    --network DIR         Network directory containing contact-plan.toml and docker-compose.yml
                          (default: contact_plan_throughput)
    --rate N              Packets/second (default: 100)
    --duration N          Sender duration in seconds (default: 30)
    --size N              Payload size in bytes (default: 512)
    --port N              UDP port (default: 5005)
    --sender-id N         Override sender node ID (e.g. 1)
    --receiver-id N       Override receiver node ID (e.g. 3)
    --wait-after N        Seconds to wait after sender finishes (default: 15)
    --no-analyze          Skip analyze.py
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


def get_dst_addr(contact_plan: Path, receiver_id: int) -> str:
    with open(contact_plan, "rb") as f:
        data = tomllib.load(f)

    edges = data.get("edges", [])
    for edge in edges:
        if edge["to"] == receiver_id:
            low, high = min(edge["from"], edge["to"]), max(edge["from"], edge["to"])
            return f"fd00:{low:02x}:{high:02x}::{receiver_id:x}"

    return None

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    p = argparse.ArgumentParser(
        description="DTN traffic generation against a running compose stack",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--network",
                   default="contact_plan_throughput", metavar="DIR")
    p.add_argument("--rate",        type=int, default=100)
    p.add_argument("--duration",    type=int, default=30)
    p.add_argument("--size",        type=int, default=512)
    p.add_argument("--port",        type=int, default=5005)
    p.add_argument("--sender-id",   type=int, required=True, metavar="N")
    p.add_argument("--receiver-id", type=int, required=True, metavar="N")
    p.add_argument("--wait-after",  type=int, default=15)
    p.add_argument("--no-analyze",  action="store_true")
    p.add_argument("--no-plots",    action="store_true")
    p.add_argument("--plot-fmt",    default="svg", choices=["pdf", "svg", "png"])
    args = p.parse_args()

    network_dir = Path(args.network)
    if not network_dir.is_absolute():
        network_dir = SCRIPT_DIR / network_dir
    contact_plan = network_dir / "contact-plan.toml"

    if not contact_plan.is_file():
        sys.exit(f"ERROR: contact plan not found: {contact_plan}")

    compose_file = network_dir / "docker-compose.yml"
    if not compose_file.is_file():
        sys.exit(f"ERROR: docker-compose.yml not found at {compose_file}\n"
                 f"       Run: python3 generate_network.py --network {network_dir}")

    test_case_number = 0
    env_file = network_dir / ".env"
    if env_file.is_file():
        for line in env_file.read_text().splitlines():
            if line.startswith("TEST_CASE_NUMBER="):
                try:
                    test_case_number = int(line.split("=", 1)[1])
                except ValueError:
                    pass

    captures_dir = network_dir / "captures" / str(test_case_number)
    captures_dir.mkdir(parents=True, exist_ok=True)

    sender_node = f"node{args.sender_id}"
    recv_node   = f"node{args.receiver_id}"
    dst_addr    = get_dst_addr(contact_plan, args.receiver_id)
    if dst_addr == None:
        sys.exit(f"ERROR: destination address not found: {args.receiver_id}")

    print()
    print(f"=== Traffic test: {network_dir.name} ===")
    print(f"    rate={args.rate} pkt/s  duration={args.duration}s"
          f"  size={args.size}B  port={args.port}")
    print(f"    Sender      : {sender_node}")
    print(f"    Receiver    : {recv_node}  (dst [{dst_addr}]:{args.port})")
    print(f"    Captures dir: {captures_dir}")
    print()

    def stop_receiver() -> None:
        subprocess.run(
            ["docker", "exec", recv_node, "sh", "-c",
             "kill $(pgrep -f traffic_recv.py) 2>/dev/null || true"],
            capture_output=True,
        )

    try:
        # -------------------------------------------------------------------------
        # Step 1 — Start receiver (background)
        # -------------------------------------------------------------------------
        print("--- [1/4] Starting traffic_recv.py ---")
        run(["docker", "exec", "-d", recv_node,
             "python3", "/repo/networks/traffic_recv.py", str(args.port),
             "--out", "/tmp/recv.csv"])
        time.sleep(1)

        # -------------------------------------------------------------------------
        # Step 2 — Run sender (foreground, blocks until done)
        # -------------------------------------------------------------------------
        print()
        print(f"--- [2/4] Running traffic_gen.py ({args.duration}s) ---")
        print(f"    {sender_node} -> [{dst_addr}]:{args.port}"
              f"  rate={args.rate} pkt/s  {args.size}B")
        run(["docker", "exec", sender_node,
             "python3", "/repo/networks/traffic_gen.py", dst_addr, str(args.port),
             "--rate",     str(args.rate),
             "--duration", str(args.duration),
             "--size",     str(args.size),
             "--out",      "/tmp/sent.csv"])
        print("Sender finished.")

        # -------------------------------------------------------------------------
        # Step 3 — Wait for drain
        # -------------------------------------------------------------------------
        print()
        print(f"--- [3/4] Waiting {args.wait_after}s for in-flight packets to drain ---")
        time.sleep(args.wait_after)

        # -------------------------------------------------------------------------
        # Step 4 — Collect artefacts
        # -------------------------------------------------------------------------
        print()
        print("--- [4/4] Collecting artefacts ---")

        stop_receiver()
        time.sleep(1)

        for container, src, name in [
            (sender_node, "/tmp/sent.csv", "sent.csv"),
            (recv_node,   "/tmp/recv.csv", "recv.csv"),
        ]:
            r = subprocess.run(
                ["docker", "cp", f"{container}:{src}", str(captures_dir / name)],
                capture_output=True,
            )
            if r.returncode != 0:
                print(f"WARNING: {name} not found in {container}")
            else:
                print(f"  {name} → {captures_dir / name}")

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
        print(f"  logs.txt → {captures_dir}/logs.txt")

        tcpdump_lines: list[str] = []
        for txt in sorted(captures_dir.glob("node*.txt")):
            for line in txt.read_text(errors="replace").splitlines():
                if len(line) >= 4 and line[:4].isdigit():
                    tcpdump_lines.append(line)
        if tcpdump_lines:
            tcpdump_lines.sort()
            (captures_dir / "tcpdump.txt").write_text("\n".join(tcpdump_lines) + "\n")
            print(f"  tcpdump.txt → {captures_dir}/tcpdump.txt")

        if not args.no_analyze:
            print()
            subprocess.run(
                [sys.executable, str(SCRIPT_DIR / "analyze.py"),
                 str(captures_dir)],
                check=False,
            )
            if not args.no_plots:
                subprocess.run(
                    [sys.executable, str(SCRIPT_DIR / "plot_metrics.py"),
                     "--captures", str(captures_dir),
                     "--fmt",      args.plot_fmt],
                    check=False,
                )

        print()
        print("=" * 60)
        print("  Traffic test complete")
        print("=" * 60)
        print(f"  Captures dir : {captures_dir}")
        print()
        print("  Files:")
        print("    sent.csv    — per-packet send timestamps")
        print("    recv.csv    — per-packet receive timestamps")
        print("    logs.txt    — docker compose logs")
        print("    tcpdump.txt — merged per-node traffic (time-sorted)")
        print("    report.*    — PDR / latency / throughput summary")
        print("    metrics.jsonl — per-container resource metrics")
        print("=" * 60)

    finally:
        stop_receiver()


if __name__ == "__main__":
    main()
