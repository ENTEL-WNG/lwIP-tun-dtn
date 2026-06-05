#!/usr/bin/env python3
"""
run_traffic.py — Traffic generation/collection for a DTN throughput experiment.

Assumes the docker compose stack is already running:
    cd networks/<plan_dir> && docker compose up -d --build

This script only:
  1. Starts traffic_recv.py inside the receiver container (background)
  2. Runs  traffic_gen.py  inside the sender container (foreground)
  3. Waits --wait-after seconds for in-flight packets to drain
  4. Collects artefacts: sent.csv, recv.csv, logs.txt, tcpdump.txt, report.*

Usage:
    python3 run_traffic.py [options]

Options:
    --contact-plan FILE   Contact plan TOML (default: networks/contact-plan-throughput.toml)
    --rate N              Packets/second (default: 100)
    --duration N          Sender duration in seconds (default: 30)
    --size N              Payload size in bytes (default: 512)
    --port N              UDP port (default: 5005)
    --sender NODE         Override sender container name
    --receiver NODE       Override receiver container name
    --relay NODES         Comma-separated relay names (override)
    --dst-addr ADDR       Override IPv6 destination address
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


def node_roles(contact_plan: Path) -> dict:
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


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    p = argparse.ArgumentParser(
        description="DTN traffic generation against a running compose stack",
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
    p.add_argument("--relay",      default="", metavar="NODES")
    p.add_argument("--dst-addr",   default="", metavar="ADDR")
    p.add_argument("--wait-after", type=int, default=15)
    p.add_argument("--no-analyze", action="store_true")
    args = p.parse_args()

    contact_plan = Path(args.contact_plan)
    if not contact_plan.is_file():
        sys.exit(f"ERROR: contact plan not found: {contact_plan}")

    # Derive compose dir and captures dir from the contact plan name
    with open(contact_plan, "rb") as f:
        plan_data = tomllib.load(f)
    plan_name    = plan_data["contact_plan"]["name"]
    compose_dir  = SCRIPT_DIR / "networks" / plan_name
    compose_file = compose_dir / "docker-compose.yml"
    if not compose_file.is_file():
        sys.exit(f"ERROR: docker-compose.yml not found at {compose_file}\n"
                 f"       Run: python3 generate-network.py {contact_plan}")

    test_case_number = 0
    env_file = compose_dir / ".env"
    if env_file.is_file():
        for line in env_file.read_text().splitlines():
            if line.startswith("TEST_CASE_NUMBER="):
                try:
                    test_case_number = int(line.split("=", 1)[1])
                except ValueError:
                    pass

    captures_dir = compose_dir / "captures" / str(test_case_number)
    captures_dir.mkdir(parents=True, exist_ok=True)

    # Node roles
    roles       = node_roles(contact_plan)
    sender_node = args.sender   or roles["sender"]
    recv_node   = args.receiver or roles["receiver"]
    relay_nodes = (
        [r.strip() for r in args.relay.split(",") if r.strip()]
        if args.relay else roles["relays"]
    )
    dst_addr = args.dst_addr or roles["dst_addr"]

    print()
    print(f"=== Traffic test: {contact_plan.name} ===")
    print(f"    rate={args.rate} pkt/s  duration={args.duration}s"
          f"  size={args.size}B  port={args.port}")
    print(f"    Sender      : {sender_node}")
    print(f"    Receiver    : {recv_node}  (dst [{dst_addr}]:{args.port})")
    print(f"    Relays      : {' '.join(relay_nodes)}")
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
                [sys.executable, str(SCRIPT_DIR / "networks/analyze.py"),
                 str(captures_dir)],
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
