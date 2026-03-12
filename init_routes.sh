#!/bin/bash
set -e

# 1. Mac/Docker stability settings
sysctl -w net.ipv6.conf.all.accept_dad=0
sysctl -w net.ipv6.conf.all.forwarding=1

echo "--- DTN $NODE_NR: Initializing Network Environment ---"

# 2. Safe Interface Renaming (Rotate through temp names)
OLD_INT1=$(ip -o link show | awk -F': ' '{print $2}' | grep '^eth' | sed 's/@.*//' | sed -n '1p')
OLD_INT2=$(ip -o link show | awk -F': ' '{print $2}' | grep '^eth' | sed 's/@.*//' | sed -n '2p')

ip link set "$OLD_INT1" down && ip link set "$OLD_INT1" name temp0
ip link set "$OLD_INT2" down && ip link set "$OLD_INT2" name temp1
ip link set temp0 name enp0s8 && ip link set enp0s8 up
ip link set temp1 name enp0s9 && ip link set enp0s9 up

# 3. Configure IPv6 (Static IPs & Routes)
echo "Configuring Node $NODE_NR static IPv6 addresses..."
# enp0s8 Config
ip -6 addr add fd00:00::1/64 dev enp0s8 || true

# enp0s9 Config (This is plugged into net_12 -> Connects to Node 2)
ip -6 addr add fd00:01::1/64 dev enp0s9 || true
ip -6 route add fd00:12::/64 via fd00:01::2 dev enp0s9 || true
ip -6 route add fd00:22::/64 via fd00:01::2 dev enp0s9 || true
ip -6 route add fd00:33::/64 via fd00:01::2 dev enp0s9 || true