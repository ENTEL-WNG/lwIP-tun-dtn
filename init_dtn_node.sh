#!/bin/bash
set -e

# 1. Give the Docker VM bridge a moment to stabilize
# sleep 2

# # 2. Flush any old settings that might have leaked from a previous run
# ip addr flush dev eth0 2>/dev/null || true
# ip addr flush dev eth1 2>/dev/null || true

# # 3. Disable DAD (Prevents the "tentative" IP lock)
# sysctl -w net.ipv6.conf.all.accept_dad=0
# sysctl -w net.ipv6.conf.all.forwarding=1

# sleep 3

# # 1. Force-disable Duplicate Address Detection (DAD)
# # This is the #1 killer of 'sometimes' working IPv6
# sysctl -w net.ipv6.conf.all.accept_dad=0
# sysctl -w net.ipv6.conf.all.forwarding=1

echo "--- DTN $NODE_NR: Initializing Network Environment ---"

# 1. Rename Docker interfaces to enp0s8 and enp0s9 to not change existing c code
# We grab the first two 'eth' interfaces found
OLD_INT1=$(ip -o link show | awk -F': ' '{print $2}' | grep '^eth' | sed 's/@.*//' | sed -n '1p')
OLD_INT2=$(ip -o link show | awk -F': ' '{print $2}' | grep '^eth' | sed 's/@.*//' | sed -n '2p')

if [ -z "$OLD_INT1" ] || [ -z "$OLD_INT2" ]; then
    echo "Error: Could not find two ethernet interfaces to rename."
    exit 1
fi

echo "Renaming $OLD_INT1 -> enp0s8 and $OLD_INT2 -> enp0s9"
ip link set "$OLD_INT1" down && ip link set "$OLD_INT1" name enp0s8 && ip link set enp0s8 up
ip link set "$OLD_INT2" down && ip link set "$OLD_INT2" name enp0s9 && ip link set enp0s9 up

# 2. Configure IPv6 (Logic from /etc/network/interfaces)
echo "Configuring $PS1 static IPv6 addresses and routes..."

# enp0s8 Config
ip -6 addr add fd00:12::1/64 dev enp0s8 || true
ip -6 route add fd00:23::/64 via fd00:12::2 dev enp0s8 || true
ip -6 route add fd00:33::/64 via fd00:12::2 dev enp0s8 || true
ip -6 route add fd00:22::/64 via fd00:12::2 dev enp0s8 || true

# enp0s9 Config
ip -6 addr add fd00:01::2/64 dev enp0s9 || true

# Enable Forwarding
sysctl -w net.ipv6.conf.all.forwarding=1

# 3. Setup tun0 (Logic from setup-tun0.sh)
echo "Setting up tun0 interface..."
mkdir -p /dev/net
[ -c /dev/net/tun ] || mknod /dev/net/tun c 10 200

ip tuntap add dev tun0 mode tun || true
ip addr add 10.0.0.1/24 dev tun0 || true
ip -6 addr add fd00::1/64 dev tun0 || true
ip link set tun0 up

# 4. IP6Tables & Policy Routing (Table 100)
echo "Applying Node 1 Policy Based Routing (Table 100)..."
ip6tables -t mangle -F PREROUTING
ip -6 rule del prio 10000 fwmark 1 table 100 2>/dev/null || true

# ip6tables -t mangle -A PREROUTING -p icmpv6 -j ACCEPT

# Mark packets from physical interfaces not destined for local machine
ip6tables -t mangle -A PREROUTING -i enp0s8 -m addrtype ! --dst-type LOCAL -j MARK --set-mark 1
ip6tables -t mangle -A PREROUTING -i enp0s9 -m addrtype ! --dst-type LOCAL -j MARK --set-mark 1

# Add routing rule if it doesn't exist
if ! ip -6 rule list | grep -q "fwmark 1.*lookup 100"; then
    ip -6 rule add fwmark 1 table 100 priority 10000
fi

# Route table 100 default via tun0 gateway
ip -6 route replace default via fd00::2 dev tun0 table 100

echo "--- Node 1 Setup Complete. Starting Binary ---"

# 5. Start the binary
exec ./lwip_tun

# docker logs -t dtn_node_1