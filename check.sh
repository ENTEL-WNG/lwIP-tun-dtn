#!/bin/bash

# 1. Check interfaces got their addresses
ip -6 addr show

# 2. Check routes were installed
ip -6 route show

ip neighbour

# 3. Check forwarding is enabled
sysctl net.ipv6.conf.all.forwarding

# 4. Check env vars loaded (needs a login shell)
sudo -i bash -c 'source /etc/profile.d/dtn_env.sh && env | grep -E "HOST_|NODE_ID|CURR_NODE|CONTACT"'
echo "gloabl env"
sudo env | grep -E "HOST_|NODE_ID|CURR_NODE|CONTACT_PLAN"

# 5. Check tun0 is up (nodes 1–3)
ip link show tun0
ip -6 addr show tun0

# Check what env the app actually sees (check node_id is 1, not 0)
sudo cat /proc/$(pgrep lwip_tun)/environ | tr '\0' '\n' | grep -E "NODE_ID|HOST_"

# Check ip6tables TEE rules are active
sudo ip6tables -t mangle -L PREROUTING -n -v

# Check table 100 route exists
ip -6 route show table 100

# Confirm tun0 is up and has fd00::1
ip -6 addr show tun0

# sudo ip6tables -t filter -F INPUT
# sudo ip6tables -t filter -A INPUT -p ipv6-icmp --icmpv6-type neighbour-solicitation -j ACCEPT
# sudo ip6tables -t filter -A INPUT -p ipv6-icmp --icmpv6-type neighbour-advertisement -j ACCEPT
# sudo ip6tables -t filter -A INPUT -d fd00:01::2 -j DROP
# sudo ip6tables -t filter -A INPUT -d fd00:12::1 -j DROP

# Watch what arrives on tun0 when you ping from node0
sudo tcpdump -i tun0 -n ip6

# ping6 -c3 fd00:01::2    # node1/enp0s9
# ping6 -c3 fd00:12::1    # node1/enp0s8 (via node1)
# ping6 -c3 fd00:12::2    # node2/enp0s8 (via node1→node2)
# ping6 -c3 fd00:23::3    # node3/enp0s8 (via node1→node2→node3)