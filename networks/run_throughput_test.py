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
  --no-plots            Skip plot_metrics.py
  --keep-up             Skip docker compose down (and skip plots)
  --plot-fmt FORMAT     Plot output format: pdf, svg, png (default: pdf)


One command, fully automated
  python3 throughput_test.py --rate 200 --duration 60 --size 1024

Skip plots during a run, generate them later
  python3 throughput_test.py --no-plots
  python3 plot_metrics.py --captures <plan_dir>/captures/1

Manual workflow (containers already up)
  cd networks/contact_plan_throughput && docker compose up -d --build
  python3 run_traffic.py --rate 200 --duration 60
  python3 plot_metrics.py --captures <plan_dir>/captures/1
"""

import argparse
import json
import shlex
import shutil
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


def wait_containers(compose_file: Path, captures_dir: Path, timeout: int = 120) -> None:
    """Poll until all containers in the compose file are running, then save inspect data."""
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
            inspect = subprocess.run(
                ["docker", "inspect"] + total,
                capture_output=True, text=True,
            )
            (captures_dir / "container_inspect.json").write_text(inspect.stdout)
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
    p.add_argument("--network",
                   default="contact_plan_throughput", metavar="DIR")
    p.add_argument("--rate",        type=int, default=100)
    p.add_argument("--duration",    type=int, default=30)
    p.add_argument("--size",        type=int, default=512)
    p.add_argument("--port",        type=int, default=5005)
    p.add_argument("--sender-id",   type=int, required=True, metavar="N")
    p.add_argument("--receiver-id", type=int, required=True, metavar="N")
    p.add_argument("--wait-after",       type=int, default=5)
    p.add_argument("--capture-interval", type=int, default=30, metavar="N",
                   help="Log capture interval in seconds (default: 30)")
    p.add_argument("--no-analyze",  action="store_true")
    p.add_argument("--no-plots",    action="store_true")
    p.add_argument("--keep-up",     action="store_true")
    p.add_argument("--plot-fmt",    default="svg", choices=["pdf", "svg", "png"])
    args = p.parse_args()

    network_dir = Path(args.network)
    if not network_dir.is_absolute():
        network_dir = SCRIPT_DIR / network_dir
    contact_plan = network_dir / "contact-plan.toml"

    if not contact_plan.is_file():
        sys.exit(f"ERROR: contact plan not found: {contact_plan}")

    print()
    print(f"=== Throughput test: {network_dir} ===")
    print(f"    rate={args.rate} pkt/s  duration={args.duration}s"
          f"  size={args.size}B  port={args.port}")
    print()

    # -------------------------------------------------------------------------
    # Step 1 — Generate network configs
    # -------------------------------------------------------------------------
    print("--- [1/5] Generating network configs ---")
    gen = subprocess.run(
        [sys.executable, str(SCRIPT_DIR / "generate_network.py"), f"{network_dir}/contact-plan.toml",
         "--capture-interval", str(args.capture_interval)],
        stdout=subprocess.PIPE, text=True, cwd=str(SCRIPT_DIR), check=True,
    )
    print(gen.stdout, end="")

    compose_file = network_dir / "docker-compose.yml"
    if not compose_file.is_file():
        sys.exit(f"ERROR: docker-compose.yml not found: {compose_file}")

    storage_dir = SCRIPT_DIR / "../dtn_storage" / args.network
    if storage_dir.exists():
        print(f"    Removing old storage: {storage_dir}")
        shutil.rmtree(storage_dir)

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
    if captures_dir.exists():
        print(f"    Removing old captures: {captures_dir}")
        shutil.rmtree(captures_dir)
    captures_dir.mkdir(parents=True)

    # -------------------------------------------------------------------------
    # Teardown helpers
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
        print("    building images...")
        build_start = time.monotonic()
        run(["docker", "compose", "-f", str(compose_file), "build"])
        build_ms = (time.monotonic() - build_start) * 1000
        (captures_dir / "build_time.json").write_text(json.dumps({"build_ms": round(build_ms)}))
        print(f"    build time: {build_ms / 1000:.1f}s")
        run(["docker", "compose", "-f", str(compose_file), "up", "-d"])

        # -------------------------------------------------------------------------
        # Step 3 — Wait for containers
        # -------------------------------------------------------------------------
        print()
        print("--- [3/5] Waiting for containers ---")
        wait_containers(compose_file, captures_dir)
        time.sleep(3)  # let lwip_tun finish initialising

        # -------------------------------------------------------------------------
        # Step 4 — Run traffic (via run_traffic.py)
        # -------------------------------------------------------------------------
        print()
        print("--- [4/5] Running traffic ---")
        traffic_cmd = [
            sys.executable, str(SCRIPT_DIR / "run_traffic.py"),
            "--network",    str(network_dir),
            "--rate",       str(args.rate),
            "--duration",   str(args.duration),
            "--size",       str(args.size),
            "--port",       str(args.port),
            "--wait-after", str(args.wait_after),
        ]
        traffic_cmd += ["--sender-id",   str(args.sender_id)]
        traffic_cmd += ["--receiver-id", str(args.receiver_id)]

        run(traffic_cmd)

        # -------------------------------------------------------------------------
        # Step 6 — Stop log collection + compose down
        # -------------------------------------------------------------------------
        print()
        print("--- [5/5] Tearing down ---")
        do_compose_down()

        # -------------------------------------------------------------------------
        # Step 7 — Analyze + plots
        # -------------------------------------------------------------------------
        if not args.no_analyze:
            print()
            print("--- Analyzing results ---")
            subprocess.run(
                [sys.executable, str(SCRIPT_DIR / "analyze.py"),
                 str(captures_dir)],
                check=False,
            )
            if not args.no_plots:
                subprocess.run(
                    [sys.executable, str(SCRIPT_DIR / "plot_metrics.py"),
                     str(captures_dir),
                     "--fmt", args.plot_fmt],
                    check=False,
                )

        print()
        print("=" * 60)
        print("  Throughput test complete")
        print("=" * 60)
        print(f"  Network      : {network_dir}")
        print(f"  Run index    : {test_case_number}")
        print(f"  Captures dir : {captures_dir}")
        print()
        print("  Files:")
        print("    sent.csv          — per-packet send timestamps")
        print("    recv.csv          — per-packet receive timestamps")
        print("    metrics.jsonl     — per-second CPU / mem / net / DB snapshots")
        print("    logs_*_HH.txt     — docker compose logs (hourly, via collect_logs.py)")
        print("    tcpdump.txt       — merged per-node traffic (via collect_logs.py)")
        print("    report.*          — PDR / latency / throughput summary")
        print(f"    plots/            — line graphs ({args.plot_fmt})")
        print("=" * 60)

    finally:
        do_compose_down()


if __name__ == "__main__":
    main()
