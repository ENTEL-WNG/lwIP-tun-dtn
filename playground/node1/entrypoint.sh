#!/bin/bash
set -e

# Create TAP interface that LwIP will bind to
ip tuntap add dev tap0 mode tap 2>/dev/null || true
ip link set tap0 up

# Assign the IPv6 address to the tap so the kernel
# knows how to route packets to/from LwIP's stack
ip -6 addr add fd00::1/64 dev tap0 2>/dev/null || true

echo "[node1] tap0 ready — starting LwIP..."
exec /app/build/lwip_node1