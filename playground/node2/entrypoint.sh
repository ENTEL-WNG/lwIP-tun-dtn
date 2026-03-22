#!/bin/bash
set -e

ip tuntap add dev tap0 mode tap 2>/dev/null || true
ip link set tap0 up
ip -6 addr add fd00::2/64 dev tap0 2>/dev/null || true

echo "[node2] waiting 2s for node1 to be ready..."
sleep 2

echo "[node2] sending ping6 to fd00::1 ..."
ping6 -c 5 -I tap0 fd00::1

echo "[node2] done"