#!/usr/bin/env python3
"""
traffic_gen.py — UDP traffic generator for DTN throughput experiments.

Sends UDP6 packets at a configurable rate for a configurable duration.
Each packet payload:
  bytes 0-3   : 32-bit sequence number (big-endian)
  bytes 4-11  : 64-bit send timestamp in microseconds since epoch (big-endian)
  bytes 12-N  : zero padding to reach --size bytes

Output CSV (--out):
  seq, sent_ts_us, size

Usage:
  python3 traffic_gen.py <dst_addr> <port> [options]

Examples:
  # Send 50 pkt/s of 512 bytes for 30 s to fd00:2:3::3:5005
  python3 traffic_gen.py fd00:2:3::3 5005 --rate 50 --duration 30 --size 512 --out /tmp/sent.csv
"""

import argparse
import csv
import socket
import struct
import time


def parse_args():
    p = argparse.ArgumentParser(description="UDP6 traffic generator")
    p.add_argument("dst_addr", help="Destination IPv6 address")
    p.add_argument("port", type=int, help="Destination UDP port")
    p.add_argument("--rate", type=float, default=10.0,
                   help="Target send rate in packets/second (default: 10)")
    p.add_argument("--duration", type=float, default=30.0,
                   help="Duration in seconds (default: 30)")
    p.add_argument("--size", type=int, default=256,
                   help="Packet payload size in bytes (default: 256, min: 12)")
    p.add_argument("--out", default="/tmp/sent.csv",
                   help="Output CSV path (default: /tmp/sent.csv)")
    p.add_argument("--src-port", type=int, default=0,
                   help="Source port; 0 = OS-assigned (default: 0)")
    return p.parse_args()


def main():
    args = parse_args()
    size = max(args.size, 12)  # need at least 12 bytes for header

    sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
    if args.src_port:
        sock.bind(('::', args.src_port))

    dst = (args.dst_addr, args.port, 0, 0)

    interval = 1.0 / args.rate
    deadline = time.monotonic() + args.duration

    records = []
    seq = 0

    print(f"[traffic_gen] sending to [{args.dst_addr}]:{args.port} "
          f"rate={args.rate} pkt/s  size={size} B  duration={args.duration} s")

    next_send = time.monotonic()
    while time.monotonic() < deadline:
        now_mono = time.monotonic()
        if now_mono < next_send:
            time.sleep(next_send - now_mono)

        ts_us = int(time.time() * 1_000_000)
        header = struct.pack(">IQ", seq, ts_us)
        padding = bytes(size - len(header))
        payload = header + padding

        try:
            sock.sendto(payload, dst)
            records.append((seq, ts_us, size))
        except OSError as e:
            print(f"[traffic_gen] send error seq={seq}: {e}")

        seq += 1
        next_send += interval

    sock.close()

    with open(args.out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["seq", "sent_ts_us", "size"])
        w.writerows(records)

    print(f"[traffic_gen] done — sent {seq} packets, wrote {args.out}")


if __name__ == "__main__":
    main()
