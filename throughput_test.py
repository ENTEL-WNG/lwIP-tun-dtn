#!/usr/bin/env python3
"""
throughput_test.py — End-to-end DTN throughput experiment.

Steps:
  1.  Generate per-node configs + docker-compose from the contact plan
  2.  docker compose up -d --build
  3.  Wait for all containers to be running
  4.  Run run_traffic.py  (receiver → sender → drain → artifact collection)
  5.  docker compose down
  6.  Generate plots with plot_metrics.py

All traffic and analysis options are forwarded verbatim to run_traffic.py.

Usage:
  python3 throughput_test.py [options]

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
  --no-plots            Skip plot_metrics.py
  --keep-up             Skip docker compose down (and skip plots)
  --plot-fmt FORMAT     Plot output format: pdf, svg, png (default: pdf)


One command, fully automated
  python3 throughput_test.py --rate 200 --duration 60 --size 1024

Skip plots during a run, generate them later
  python3 throughput_test.py --no-plots
  python3 networks/plot_metrics.py --captures networks/contact_plan_throughput/captures/1

Manual workflow (containers already up)
  cd networks/contact_plan_throughput && docker compose up -d --build
  python3 run_traffic.py --rate 200 --duration 60
  python3 networks/plot_metrics.py --captures networks/contact_plan_throughput/captures/1
"""

import argparse
import shlex
import subprocess
import sys
import time
from pathlib import Path

SCRIPT_DIR = Path(__file__).parent.resolve()


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def run(cmd: list, **kwargs) -> subprocess.CompletedProcess:
    print(f"  $ {shlex.join(str(c) for c in cmd)}")
    return subprocess.run(cmd, check=True, **kwargs)


def wait_containers(compose_file: Path, timeout: int = 120) -> None:
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
            print(f"    All {len(total)} container(s) running.")
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
    p.add_argument("--relay",      default="", metavar="NODES")
    p.add_argument("--dst-addr",   default="", metavar="ADDR")
    p.add_argument("--wait-after", type=int, default=5)
    p.add_argument("--no-analyze", action="store_true")
    p.add_argument("--no-plots",   action="store_true")
    p.add_argument("--keep-up",    action="store_true")
    p.add_argument("--plot-fmt",   default="svg", choices=["pdf", "svg", "png"])
    args = p.parse_args()

    contact_plan = Path(args.contact_plan)
    if not contact_plan.is_file():
        sys.exit(f"ERROR: contact plan not found: {contact_plan}")

    print()
    print(f"=== Throughput test: {contact_plan.name} ===")
    print(f"    rate={args.rate} pkt/s  duration={args.duration}s"
          f"  size={args.size}B  port={args.port}")
    print()

    # -------------------------------------------------------------------------
    # Step 1 — Generate network configs
    # -------------------------------------------------------------------------
    print("--- [1/5] Generating network configs ---")
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

    # -------------------------------------------------------------------------
    # Teardown helper
    # -------------------------------------------------------------------------
    _compose_down_done = False

    def do_compose_down() -> None:
        nonlocal _compose_down_done
        if _compose_down_done or args.keep_up:
            return
        _compose_down_done = True
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
        print("--- [2/5] Starting docker compose ---")
        run(["docker", "compose", "-f", str(compose_file), "up", "-d", "--build"])

        # -------------------------------------------------------------------------
        # Step 3 — Wait for containers
        # -------------------------------------------------------------------------
        print()
        print("--- [3/5] Waiting for containers ---")
        wait_containers(compose_file)
        time.sleep(3)  # let lwip_tun finish initialising
        print("    Grafana   : http://localhost:3000  (admin/admin)")
        print("    Prometheus: http://localhost:9090")

        # -------------------------------------------------------------------------
        # Step 4 — Run traffic (via run_traffic.py)
        # -------------------------------------------------------------------------
        print()
        print("--- [4/5] Running traffic ---")
        traffic_cmd = [
            sys.executable, str(SCRIPT_DIR / "run_traffic.py"),
            "--contact-plan", str(contact_plan),
            "--rate",         str(args.rate),
            "--duration",     str(args.duration),
            "--size",         str(args.size),
            "--port",         str(args.port),
            "--wait-after",   str(args.wait_after),
        ]
        if args.sender:
            traffic_cmd += ["--sender",   args.sender]
        if args.receiver:
            traffic_cmd += ["--receiver", args.receiver]
        if args.relay:
            traffic_cmd += ["--relay",    args.relay]
        if args.dst_addr:
            traffic_cmd += ["--dst-addr", args.dst_addr]
        if args.no_analyze:
            traffic_cmd += ["--no-analyze"]
        if args.no_plots:
            traffic_cmd += ["--no-plots"]
        traffic_cmd += ["--plot-fmt", args.plot_fmt]

        run(traffic_cmd)

        # -------------------------------------------------------------------------
        # Step 5 — Compose down + plots
        # -------------------------------------------------------------------------
        print()
        print("--- [5/5] Tearing down ---")
        do_compose_down()

        print()
        print("=" * 60)
        print("  Throughput test complete")
        print("=" * 60)
        print(f"  Contact plan : {contact_plan.name}")
        print(f"  Run index    : {test_case_number}")
        print(f"  Captures dir : {captures_dir}")
        print()
        print("  Files:")
        print("    sent.csv       — per-packet send timestamps")
        print("    recv.csv       — per-packet receive timestamps")
        print("    metrics.jsonl  — per-second CPU / mem / net / DB snapshots")
        print("    logs.txt       — docker compose logs")
        print("    tcpdump.txt    — merged per-node traffic (time-sorted)")
        print("    report.*       — PDR / latency / throughput summary")
        print(f"    plots/         — line graphs ({args.plot_fmt})")
        print("=" * 60)

    finally:
        do_compose_down()


if __name__ == "__main__":
    main()
