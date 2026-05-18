#!/bin/bash
# set -e

echo "$(date +"[%Y-%m-%d %H:%M:%S.%3N]") --- Initializing Docker Network Environment ---"

# --- 2. Kernel Settings ---
sysctl -w net.ipv6.conf.all.accept_dad=0 || true
sysctl -w net.ipv6.conf.all.forwarding=1 || true

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


modprobe nf_log_ipv6 || true
sysctl -w net.netfilter.nf_log.10=nf_log_ipv6 || true

ip6tables -t raw -F PREROUTING
ip6tables -t raw -F OUTPUT
ip6tables -t mangle -F PREROUTING
ip6tables -t mangle -F OUTPUT
ip6tables -t filter -F OUTPUT

ip6tables -t raw -A PREROUTING -i enp0s8 -j TRACE
ip6tables -t raw -A OUTPUT -j TRACE

for TARGET in 133 134 135 136; do
    ip6tables -t mangle -A PREROUTING -p icmpv6 --icmpv6-type $TARGET -j ACCEPT
done

for interface in "${ALL_INTERFACES[@]}"; do
    echo "ip6tables -t mangle -A PREROUTING -i "$interface" -j MARK --set-mark 1"
    ip6tables -t mangle -A PREROUTING -i "$interface" -j MARK --set-mark 1
done

ip6tables -t mangle -A OUTPUT -j MARK --set-mark 1

ip -6 rule del fwmark 1 table 100 2>/dev/null || true
ip -6 rule add fwmark 1 table 100 priority 0

ip -6 route replace default via "$HOST_LWIP_IPV6_ADDR" dev tun0 table 100

if ip -6 rule show | grep -q "^0:.*lookup local"; then
    ip -6 rule add pref 1000 table local 2>/dev/null || true
    ip -6 rule del pref 0 2>/dev/null || true
fi

echo "--- Setup Complete. Starting LwIP binary ---"
sleep 2
exec stdbuf -oL -eL ./lwip_tun 2>&1