#!/bin/bash
set -e

# --- 1. Environment Variables (with safe defaults) ---
HOST_TUN_IPV6_ADDR=${HOST_TUN_IPV6_ADDR:-"fd00:22::1"}
HOST_LWIP_IPV6_ADDR=${HOST_LWIP_IPV6_ADDR:-"fd00:22::2"}
INT_1=${INT_1:-"enp0s8"}
INT_2=${INT_2:-"enp0s9"}

sysctl -w net.ipv6.conf.all.accept_dad=0
sysctl -w net.ipv6.conf.all.forwarding=1

echo "--- DTN $NODE: Initializing Network Environment ---"

# --- 2. Interface Renaming ---
OLD_INT1=$(ip -o link show | awk -F': ' '{print $2}' | grep '^eth' | sed 's/@.*//' | sed -n '1p')
OLD_INT2=$(ip -o link show | awk -F': ' '{print $2}' | grep '^eth' | sed 's/@.*//' | sed -n '2p')

if [ -n "$OLD_INT1" ] && [ -n "$OLD_INT2" ]; then
    echo "Renaming Docker interfaces: $OLD_INT1->$INT_1, $OLD_INT2->$INT_2"
    ip link set "$OLD_INT1" down && ip link set "$OLD_INT1" name temp0
    ip link set "$OLD_INT2" down && ip link set "$OLD_INT2" name temp1
    ip link set temp0 name "$INT_1" && ip link set "$INT_1" up
    ip link set temp1 name "$INT_2" && ip link set "$INT_2" up
else
    echo "Warning: Could not find two eth interfaces to rename. Using existing names."
    INT_1=$OLD_INT1
    INT_2=$OLD_INT2
fi

# --- 3. Configure IPv6 ---
ip -6 addr add fd00:12::2/64 dev $INT_1 || true
ip -6 route add fd00::/64    via fd00:12::1 dev $INT_1 || true
ip -6 route add fd00:01::/64 via fd00:12::1 dev $INT_1 || true

ip -6 addr add fd00:23::2/64 dev $INT_2 || true
ip -6 route add fd00:33::/64 via fd00:23::3 dev $INT_2 || true

# --- 4. Setup tun0 ---
mkdir -p /dev/net
[ -c /dev/net/tun ] || mknod /dev/net/tun c 10 200

ip tuntap add dev tun0 mode tun || true
ip -6 addr add "$HOST_TUN_IPV6_ADDR/64" dev tun0 || true
ip link set tun0 up

# --- 5. Policy Routing ---
ip6tables -t mangle -F PREROUTING

# ONLY exempt Neighbor Discovery (RS/RA/NS/NA) — NOT echo request/reply
# Echo request (128) and echo reply (129) must be marked so they reach LwIP
for TYPE in 133 134 135 136; do
    ip6tables -t mangle -A PREROUTING -p icmpv6 --icmpv6-type $TYPE -j ACCEPT
done

# Mark ALL traffic from physical interfaces (not just non-LOCAL destinations)
# The ND exemption above already protects neighbour resolution
ip6tables -t mangle -A PREROUTING -i "$INT_1" -j MARK --set-mark 1
ip6tables -t mangle -A PREROUTING -i "$INT_2" -j MARK --set-mark 1
# ip6tables -t mangle -A PREROUTING -i "$INT_1" -m addrtype ! --dst-type LOCAL -j MARK --set-mark 1
# ip6tables -t mangle -A PREROUTING -i "$INT_2" -m addrtype ! --dst-type LOCAL -j MARK --set-mark 1

ip -6 rule del fwmark 1 table 100 2>/dev/null || true
ip -6 rule add fwmark 1 table 100 priority 10000
ip -6 route replace default via "$HOST_LWIP_IPV6_ADDR" dev tun0 table 100

# Docker fix: move local table below our rule so it doesn't intercept first
if ip -6 rule show | grep -q "^0:.*lookup local"; then
    ip -6 rule add pref 32000 table local 2>/dev/null || true
    ip -6 rule del pref 0 2>/dev/null || true
fi

echo "--- Node 2 Setup Complete. Starting Binary ---"

exec stdbuf -oL -eL ./lwip_tun 2>&1