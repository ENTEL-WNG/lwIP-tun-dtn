## Node 0:
### For /etc/network/interfaces we have to add:

auto enp0s8
iface enp0s8 inet6 static
address fd00:01::1
netmask 64
gateway fd00:01::2
post-up sysctl -w net.ipv6.conf.all.forwarding=1
post-up ip -6 route add fd00:12::/64 via fd00:01::2
post-up ip -6 route add fd00::/64 via fd00:01::2
post-up ip -6 route add fd00:22::/64 via fd00:01::2
post-up ip -6 route add fd00:33::/64 via fd00:01::2

## Node 1:
### For /etc/network/interfaces we have to add:

auto enp0s8
iface enp0s8 inet6 static
address fd00:12::1
netmask 64
gateway fd00:12::2
post-up ip -6 route add fd00:23::/64 via fd00:12::2
post-up ip -6 route add fd00:33::/64 via fd00:12::2
post-up ip -6 route add fd00:22::/64 via fd00:12::2

auto enp0s9
iface enp0s9 inet6 static
address fd00:01::2
netmask 64

post-up sysctl -w net.ipv6.conf.all.forwarding=1


### We create /etc/systemd/system/tun0.service with:
[Unit]
Description=Set up tun0 interface
After=network.target
[Service]
Type=oneshot
ExecStart=/usr/local/sbin/setup-tun0.sh
ExecStop=/usr/bin/ip link delete tun0
RemainAfterExit=yes
[Install]
WantedBy=multi-user.target


And we create /usr/local/sbin/setup-tun0.sh with:
#!/bin/bash
set -e
ip tuntap add dev tun0 mode tun || true
ip addr add 10.0.0.1/24 dev tun0 || true
ip -6 addr add fd00::1/64 dev tun0 || true
ip link set tun0 up
ip6tables -t mangle -F PREROUTING
ip -6 rule del prio 10000 fwmark 1 table 100 2>/dev/null || true
ip6tables -t mangle -A PREROUTING -i enp0s8 -m addrtype ! --dst-type LOCAL -j MARK --set-
mark 1
ip6tables -t mangle -A PREROUTING -i enp0s9 -m addrtype ! --dst-type LOCAL -j MARK --set-
mark 1
if ! ip -6 rule list | grep -q "fwmark 1.*lookup 100"; then
ip -6 rule add fwmark 1 table 100 priority 10000
fi
ip -6 route replace default via fd00::2 dev tun0 table 100

# Node 2:
/etc/network/interfaces:
auto enp0s8
iface enp0s8 inet6 static
address fd00:12::2
netmask 64
post-up ip -6 route add fd00::/64 via fd00:12::1
post-up ip -6 route add fd00:01::/64 via fd00:12::1

auto enp0s9
iface enp0s9 inet6 static
address fd00:23::2
netmask 64
post-up ip -6 route add fd00:33::/64 via fd00:23::3
post-up sysctl -w net.ipv6.conf.all.forwarding=1
/etc/systemd/system/tun0.service:
[Unit]
Description=Set up tun0 interface
After=network.target
[Service]
Type=oneshot
ExecStart=/usr/local/sbin/setup-tun0.sh
ExecStop=/usr/bin/ip link delete tun0
RemainAfterExit=yes
[Install]
WantedBy=multi-user.target
/usr/local/sbin/setup-tun0.sh:
#!/bin/bash
set -e
ip tuntap add dev tun0 mode tun || true
ip addr add 10.0.2.1/24 dev tun0 || true
ip -6 addr add fd00:22::1/64 dev tun0 || true
ip link set tun0 up
ip6tables -t mangle -F PREROUTING
ip -6 rule del prio 10000 fwmark 1 table 100 2>/dev/null || true
ip6tables -t mangle -A PREROUTING -i enp0s8 -m addrtype ! --dst-type LOCAL -j MARK --set-
mark 1
ip6tables -t mangle -A PREROUTING -i enp0s9 -m addrtype ! --dst-type LOCAL -j MARK --set-
mark 1
if ! ip -6 rule list | grep -q "fwmark 1.*lookup 100"; then
ip -6 rule add fwmark 1 table 100 priority 10000
fi
ip -6 route replace default via fd00:22::2 dev tun0 table 100


# Node 3:
/etc/network/interfaces:
auto enp0s8
iface enp0s8 inet6 static
address fd00:23::3
netmask 64
gateway fd00:23::2
post-up ip -6 route add fd00::/64 via fd00:23::2
post-up ip -6 route add fd00:01::/64 via fd00:23::2
post-up ip -6 route add fd00:12::/64 via fd00:23::2
post-up ip -6 route add fd00:22::/64 via fd00:23::2
post-up sysctl -w net.ipv6.conf.all.forwarding=1
/etc/systemd/system/tun0.service:
[Unit]
Description=Set up tun0 interface
After=network.target
[Service]
Type=oneshot
ExecStart=/usr/local/sbin/setup-tun0.sh
ExecStop=/usr/bin/ip link delete tun0
RemainAfterExit=yes
[Install]
WantedBy=multi-user.target
/usr/local/sbin/setup-tun0.sh:
#!/bin/bash
set -e
ip tuntap add dev tun0 mode tun || true
ip addr add 10.0.0.2/24 dev tun0 || true
ip -6 addr add fd00:33::1/64 dev tun0 || true
ip link set tun0 up
ip6tables -t mangle -F PREROUTING
ip -6 rule del prio 10000 fwmark 1 table 100 2>/dev/null || true
ip6tables -t mangle -A PREROUTING -i enp0s8 -m addrtype ! --dst-type LOCAL -j MARK --set-
mark 1
if ! ip -6 rule list | grep -q "fwmark 1.*lookup 100"; then
ip -6 rule add fwmark 1 table 100 priority 10000
fi
ip -6 route replace default via fd00:33::2 dev tun0 table 100