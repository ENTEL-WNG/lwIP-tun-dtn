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
ip -6 addr add fd00:01::2/64 dev enp0s8 || true

# enp0s9 Config (This is plugged into net_12 -> Connects to Node 2)
ip -6 addr add fd00:12::1/64 dev enp0s9 || true
ip -6 route add fd00:23::/64 via fd00:12::2 dev enp0s9 || true
ip -6 route add fd00:33::/64 via fd00:12::2 dev enp0s9 || true
ip -6 route add fd00:22::/64 via fd00:12::2 dev enp0s9 || true

# 4. Setup tun0
echo "Setting up tun0 interface..."
mkdir -p /dev/net
[ -c /dev/net/tun ] || mknod /dev/net/tun c 10 200

ip tuntap add dev tun0 mode tun || true
ip addr add 10.0.0.1/24 dev tun0 || true
ip -6 addr add fd00::1/64 dev tun0 || true
ip link set tun0 up

# 5. IP6Tables & Policy Routing (Table 100)
echo "Applying Node $NODE_NR Policy Based Routing..."
ip6tables -t mangle -F PREROUTING

# CRITICAL: Bypass Neighbor Discovery so ping/ARP works
# ip6tables -t mangle -A PREROUTING -p icmpv6 -j ACCEPT

# Mark packets from physical interfaces not destined for local machine
ip6tables -t mangle -A PREROUTING -i enp0s8 -m addrtype ! --dst-type LOCAL -j MARK --set-mark 1
ip6tables -t mangle -A PREROUTING -i enp0s9 -m addrtype ! --dst-type LOCAL -j MARK --set-mark 1

# Route table 100 default via tun0 gateway
ip -6 rule del prio 10000 fwmark 1 table 100 2>/dev/null || true
ip -6 rule add fwmark 1 table 100 priority 10000
ip -6 route replace default via fd00::2 dev tun0 table 100

echo "--- Node $NODE_NR Setup Complete. Starting Binary ---"
exec stdbuf -oL -eL ./lwip_tun 2>&1