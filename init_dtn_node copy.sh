#!/bin/bash
set -e

sysctl -w net.ipv6.conf.all.accept_dad=0
sysctl -w net.ipv6.conf.all.forwarding=1

echo "--- DTN $NODE ($APP_NAME): Initializing ---"

# --- Identify interfaces by which Docker subnet they're on ---
# Docker pre-assigns addresses from the IPAM subnets we defined
detect_iface() {
    local prefix=$1
    ip -6 addr show | awk "/$prefix/{print \$NF}" | head -1
}

# Wait briefly for Docker to finish address assignment
sleep 1

RAW_NET01=$(detect_iface "fd00:01:")
RAW_NET12=$(detect_iface "fd00:12:")

if [ -z "$RAW_NET01" ] || [ -z "$RAW_NET12" ]; then
    echo "ERROR: Could not identify interfaces by subnet. Dumping addresses:"
    ip -6 addr show
    exit 1
fi

echo "Detected: net_01=$RAW_NET01, net_12=$RAW_NET12"

INT_1="enp0s8"   # will face net_01
INT_2="enp0s9"   # will face net_12

# Rename to stable names via temp to avoid conflicts
ip link set "$RAW_NET01" down && ip link set "$RAW_NET01" name temp0
ip link set "$RAW_NET12" down && ip link set "$RAW_NET12" name temp1
ip link set temp0 name "$INT_1" && ip link set "$INT_1" up
ip link set temp1 name "$INT_2" && ip link set "$INT_2" up

# --- Configure IPv6 ---
# Remove Docker's auto-assigned addresses — we use our own static ones
ip -6 addr flush dev "$INT_1" scope global || true
ip -6 addr flush dev "$INT_2" scope global || true

ip -6 addr add fd00:01::1/64 dev "$INT_1" || true
ip -6 addr add fd00:12::1/64 dev "$INT_2" || true

# Routes to reach Node 2's networks via net_12
ip -6 route add fd00:23::/64 via fd00:12::2 dev "$INT_2" || true
ip -6 route add fd00:22::/64 via fd00:12::2 dev "$INT_2" || true

# --- Setup tun0 ---
mkdir -p /dev/net
[ -c /dev/net/tun ] || mknod /dev/net/tun c 10 200
ip tuntap add dev tun0 mode tun || true
ip -6 addr add "${HOST_TUN_IPV6_ADDR}/64" dev tun0 || true
ip link set tun0 up

# --- Policy routing ---
ip6tables -t mangle -F PREROUTING

for TYPE in 133 134 135 136; do
    ip6tables -t mangle -A PREROUTING -p icmpv6 --icmpv6-type $TYPE -j ACCEPT
done

ip6tables -t mangle -A PREROUTING -i "$INT_1" -j MARK --set-mark 1
ip6tables -t mangle -A PREROUTING -i "$INT_2" -j MARK --set-mark 1

ip -6 rule del fwmark 1 table 100 2>/dev/null || true
ip -6 rule add fwmark 1 table 100 priority 10000
ip -6 route replace default via "$HOST_LWIP_IPV6_ADDR" dev tun0 table 100

if ip -6 rule show | grep -q "priority: 0\|pref 0"; then
    ip -6 rule add pref 32000 table local 2>/dev/null || true
    ip -6 rule del pref 0 2>/dev/null || true
fi

echo "--- Node $NODE ($APP_NAME) setup complete. Starting binary ---"
sleep 1
exec stdbuf -oL -eL ./lwip_tun 2>&1