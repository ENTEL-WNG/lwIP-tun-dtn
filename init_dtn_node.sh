#!/bin/bash
set -e

# --- 1. Environment Variables ---
# HOST_TUN_IPV6_ADDR=${HOST_TUN_IPV6_ADDR:-"fd00::1"}
# HOST_LWIP_IPV6_ADDR=${HOST_LWIP_IPV6_ADDR:-"fd00::2"}
# INT_1=${INT_1:-"enp0s8"}
# INT_2=${INT_2:-"enp0s9"}

# Print it or write it to a log file
echo "$(date +"[%Y-%m-%d %H:%M:%S.%3N]") --- Initializing Docker Network Environment ---"

# --- 2. Kernel Settings ---
# Note: rp_filter is skipped for IPv6 as it's not a standard global sysctl
sysctl -w net.ipv6.conf.all.accept_dad=0 || true
sysctl -w net.ipv6.conf.all.forwarding=1 || true

# sysctl -w net.ipv6.icmp.echo_ignore_all=1

# --- 3. Interface Renaming (Docker Workaround) ---
# We grab the first two 'eth' interfaces provided by Docker
# OLD_INT1=$(ip -o link show | awk -F': ' '{print $2}' | grep '^eth' | sed 's/@.*//' | sed -n '1p')
# OLD_INT2=$(ip -o link show | awk -F': ' '{print $2}' | grep '^eth' | sed 's/@.*//' | sed -n '2p')

# if [ -n "$OLD_INT1" ] && [ -n "$OLD_INT2" ]; then
#     echo "Renaming Docker interfaces: $OLD_INT1->$INT_1, $OLD_INT2->$INT_2"
#     ip link set "$OLD_INT1" down && ip link set "$OLD_INT1" name temp0
#     ip link set "$OLD_INT2" down && ip link set "$OLD_INT2" name temp1
#     ip link set temp0 name "$INT_1" && ip link set "$INT_1" up
#     ip link set temp1 name "$INT_2" && ip link set "$INT_2" up
# else
#     echo "Warning: Could not find two eth interfaces to rename. Using existing names."
#     INT_1=$OLD_INT1
#     INT_2=$OLD_INT2
# fi

# ip -6 addr add fd00:01::2/64 dev $INT_1 || true

# # enp0s9 Config (This is plugged into net_12 -> Connects to Node 2)
# ip -6 addr add fd00:12::1/64 dev $INT_2 || true
# ip -6 route add fd00:22::/64 via fd00:12::2 dev $INT_2 || true
# ip -6 route add fd00:23::/64 via fd00:12::2 dev $INT_2 || true
# ip -6 route add fd00:33::/64 via fd00:12::2 dev $INT_2 || true

source ./init_routes.sh

# --- 4. Setup TUN Interface ---
echo "Setting up /dev/net/tun..."
mkdir -p /dev/net
[ -c /dev/net/tun ] || mknod /dev/net/tun c 10 200

# Create the tunnel that LwIP will attach to
ip tuntap add dev tun0 mode tun || true
ip -6 addr add "$HOST_TUN_IPV6_ADDR/64" dev tun0 || true
ip link set tun0 up

# --- 5. Policy Routing & Mangle Rules ---
echo "Applying Policy Based Routing (Force all traffic -> LwIP)..."

# Clear tables to ensure idempotency

modprobe nf_log_ipv6 || true
sysctl -w net.netfilter.nf_log.10=nf_log_ipv6 || true

ip6tables -t raw -F PREROUTING
ip6tables -t raw -F OUTPUT
ip6tables -t mangle -F PREROUTING
ip6tables -t mangle -F OUTPUT
ip6tables -t filter -F OUTPUT

ip6tables -t raw -A PREROUTING -i enp0s8 -j TRACE
ip6tables -t raw -A OUTPUT -j TRACE

# A. EXEMPTIONS: Keep Neighbor Discovery (Layer 2) inside the Linux Kernel
for TARGET in 133 134 135 136; do
    ip6tables -t mangle -A PREROUTING -p icmpv6 --icmpv6-type $TARGET -j ACCEPT
done

# B. EXEMPTIONS: Allow kernel ND on OUTPUT too
# for TARGET in 133 134 135 136; do
#     ip6tables -t filter -A OUTPUT -p icmpv6 --icmpv6-type $TARGET -j ACCEPT  # ← add
# done

# ip6tables -t mangle -A PREROUTING -d [local_ip] -j TEE  -gateway [lwip_ip]

for interface in "${ALL_INTERFACES[@]}"; do
    echo "ip6tables -t mangle -A PREROUTING -i "$interface" -j MARK --set-mark 1"
    ip6tables -t mangle -A PREROUTING -i "$interface" -j MARK --set-mark 1
    # echo "ip6tables -t mangle -A PREROUTING -i "$interface" -m addrtype ! --dst-type LOCAL -j MARK --set-mark 1"
    # ip6tables -t mangle -A PREROUTING -i "$interface" -m addrtype ! --dst-type LOCAL -j MARK --set-mark 1
    # ip6tables -t mangle -A PREROUTING -i "$interface" -m addrtype --dst-type LOCAL -j MARK --set-mark 1
done

ip6tables -t mangle -A OUTPUT -j MARK --set-mark 1


# ip6tables -t mangle -A OUTPUT -o tun0 -j MARK --set-mark 1
# for interface in "${ALL_INTERFACES[@]}"; do
#     echo "ip6tables -t raw -A OUTPUT -o "$interface" -p icmpv6 --icmpv6-type echo-reply -j DROP"
#     ip6tables -t raw -A OUTPUT -o "$interface" -p icmpv6 --icmpv6-type echo-reply -j DROP
# done

# ip6tables -t mangle -A OUTPUT -o tun0 -j MARK --set-mark 1
# for interface in "${ALL_INTERFACES[@]}"; do
#     echo "ip6tables -t filter -A OUTPUT -o "$interface" -m mark ! --mark 1 -j DROP"
#     ip6tables -t filter -A OUTPUT -o "$interface" -m mark ! --mark 1 -j DROP
# done


# C. THE RULE: Packets with Mark 1 follow Table 100
ip -6 rule del fwmark 1 table 100 2>/dev/null || true
ip -6 rule add fwmark 1 table 100 priority 0

# D. THE ROUTE: Table 100 sends everything to LwIP
ip -6 route replace default via "$HOST_LWIP_IPV6_ADDR" dev tun0 table 100

# --- 6. The "Docker Fix" for the Local Table ---
# Docker often relies on the 'local' table (priority 0) which catches traffic
# before our rules. We move it to a lower priority.
if ip -6 rule show | grep -q "^0:.*lookup local"; then
    ip -6 rule add pref 1000 table local 2>/dev/null || true
    ip -6 rule del pref 0 2>/dev/null || true
fi

echo "--- Setup Complete. Starting LwIP binary ---"
sleep 2
exec stdbuf -oL -eL ./lwip_tun 2>&1