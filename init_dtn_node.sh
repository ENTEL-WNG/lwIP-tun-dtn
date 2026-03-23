#!/bin/bash
set -e

# --- 1. Environment Variables ---
HOST_TUN_IPV6_ADDR=${HOST_TUN_IPV6_ADDR:-"fd00::1"}
HOST_LWIP_IPV6_ADDR=${HOST_LWIP_IPV6_ADDR:-"fd00::2"}
INT_1=${INT_1:-"enp0s8"}
INT_2=${INT_2:-"enp0s9"}

echo "--- Initializing Docker Network Environment ---"

# --- 2. Kernel Settings ---
# Note: rp_filter is skipped for IPv6 as it's not a standard global sysctl
sysctl -w net.ipv6.conf.all.accept_dad=0 || true
sysctl -w net.ipv6.conf.all.forwarding=1 || true

# --- 3. Interface Renaming (Docker Workaround) ---
# We grab the first two 'eth' interfaces provided by Docker
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

ip -6 addr add fd00:01::2/64 dev $INT_1 || true

# enp0s9 Config (This is plugged into net_12 -> Connects to Node 2)
ip -6 addr add fd00:12::1/64 dev $INT_2 || true
ip -6 route add fd00:23::/64 via fd00:12::2 dev $INT_2 || true
ip -6 route add fd00:33::/64 via fd00:12::2 dev $INT_2 || true
ip -6 route add fd00:22::/64 via fd00:12::2 dev $INT_2 || true

# --- 4. Setup TUN Interface ---
echo "Setting up /dev/net/tun..."
mkdir -p /dev/net
[ -c /dev/net/tun ] || mknod /dev/net/tun c 10 200

# Create the tunnel that LwIP will attach to
ip tuntap add dev tun0 mode tun || true
ip -6 addr add "$HOST_TUN_IPV6_ADDR/64" dev tun0 || true
ip link set tun0 up

# --- 5. Policy Routing & Mangle Rules ---
echo "Applying Policy Based Routing (Force all transit -> LwIP)..."

# Clear tables to ensure idempotency
ip6tables -t mangle -F PREROUTING

# A. EXEMPTIONS: Keep Neighbor Discovery (Layer 2) inside the Linux Kernel
# If you don't do this, Linux won't be able to resolve MAC addresses.
for TARGET in 133 134 135 136; do
    ip6tables -t mangle -A PREROUTING -p icmpv6 --icmpv6-type $TARGET -j ACCEPT
done

# B. MARKING: Mark all traffic entering from physical interfaces
# ip6tables -t mangle -A PREROUTING -i "$INT_1" -j MARK --set-mark 1
# ip6tables -t mangle -A PREROUTING -i "$INT_2" -j MARK --set-mark 1
# Replace the blanket MARK rules with destination-aware ones
# Mark traffic from physical interfaces ONLY if dst is not local
ip6tables -t mangle -A PREROUTING -i "$INT_1" -m addrtype ! --dst-type LOCAL -j MARK --set-mark 1
ip6tables -t mangle -A PREROUTING -i "$INT_2" -m addrtype ! --dst-type LOCAL -j MARK --set-mark 1

# C. THE RULE: Packets with Mark 1 follow Table 100
ip -6 rule del fwmark 1 table 100 2>/dev/null || true
ip -6 rule add fwmark 1 table 100 priority 10000

# D. THE ROUTE: Table 100 sends everything to LwIP
ip -6 route replace default via "$HOST_LWIP_IPV6_ADDR" dev tun0 table 100

# --- 6. The "Docker Fix" for the Local Table ---
# Docker often relies on the 'local' table (priority 0) which catches traffic
# before our rules. We move it to a lower priority.
if ip -6 rule show | grep -q "^0:.*lookup local"; then
    ip -6 rule add pref 32000 table local 2>/dev/null || true
    ip -6 rule del pref 0 2>/dev/null || true
fi

echo "--- Setup Complete. Starting LwIP binary ---"
exec stdbuf -oL -eL ./lwip_tun 2>&1