#!/usr/bin/env python3
"""
traffic_recv.py — UDP traffic receiver for DTN throughput experiments.

Listens on a UDP6 port, records (seq, recv_ts_us) for every packet received,
and writes a CSV on SIGTERM / SIGINT / KeyboardInterrupt.

Expected packet layout (must match traffic_gen.py):
  bytes 0-3  : 32-bit sequence number (big-endian)
  bytes 4-11 : 64-bit send timestamp in microseconds since epoch (big-endian)

Output CSV (--out):
  seq, recv_ts_us

Usage:
  python3 traffic_recv.py <port> [options]

Examples:
  python3 traffic_recv.py 5005 --out /tmp/recv.csv
  python3 traffic_recv.py 5005 --bind fd00:2:3::3 --out /tmp/recv.csv
"""

import argparse
import csv
import signal
import socket
import struct
import sys
import time


def parse_args():
    p = argparse.ArgumentParser(description="UDP6 traffic receiver")
    p.add_argument("port", type=int, help="UDP port to listen on")
    p.add_argument("--bind", default="",
                   help="IPv6 address to bind to (default: all interfaces)")
    p.add_argument("--out", default="/tmp/recv.csv",
                   help="Output CSV path (default: /tmp/recv.csv)")
    p.add_argument("--buf", type=int, default=65536,
                   help="Receive buffer size in bytes (default: 65536)")
    return p.parse_args()


records = []


def write_csv(path: str) -> None:
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["seq", "recv_ts_us"])
        w.writerows(records)
    print(f"[traffic_recv] wrote {len(records)} records to {path}")


def main():
    args = parse_args()

    sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    sock.bind((args.bind, args.port))
    sock.settimeout(1.0)

    def _shutdown(signum, frame):
        print(f"\n[traffic_recv] signal {signum} received — flushing CSV")
        write_csv(args.out)
        sys.exit(0)

    signal.signal(signal.SIGTERM, _shutdown)
    signal.signal(signal.SIGINT, _shutdown)

    print(f"[traffic_recv] listening on port {args.port}")

    try:
        while True:
            try:
                data, _ = sock.recvfrom(args.buf)
            except socket.timeout:
                continue

            recv_ts_us = int(time.time() * 1_000_000)

            if len(data) < 12:
                continue  # too short to parse header

            seq, _sent_ts = struct.unpack_from(">IQ", data, 0)
            records.append((seq, recv_ts_us))

    except KeyboardInterrupt:
        pass
    finally:
        sock.close()
        write_csv(args.out)


if __name__ == "__main__":
    main()
