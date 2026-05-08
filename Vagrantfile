# -*- mode: ruby -*-
# vi: set ft=ruby :
#
# Vagrantfile for IPv6-DTN Testbed
# Topology: Node0 (Regular IPv6) <-> Node1 (DTN) <-> Node2 (DTN) <-> Node3 (DTN Destination)
#
# Subnets:
#   net01: fd00:01::/64  (Node0 <-> Node1)
#   net12: fd00:12::/64  (Node1 <-> Node2)
#   net23: fd00:23::/64  (Node2 <-> Node3)

CONTACT_PLAN_PATH = "py_cgr/contact_plans/cgr_tutorial_1.txt"

Vagrant.configure("2") do |config|

  # Use a base box with Ubuntu (adjust to your preferred box)
  config.vm.box = "ubuntu/focal64"
#   config.vm.box = "ubuntu/jammy64"

  # Disable default synced folder to keep things clean
  config.vm.synced_folder ".", "/home/vagrant/app", colons: true

  # ─────────────────────────────────────────────
  # NODE 0 — Regular IPv6 node
  # Interfaces:
  #   Adapter 1 (NAT)    — eth0 / enp0s3 (keep active, required)
  #   Adapter 2 (net01)  — enp0s8  fd00:01::1/64, gw fd00:01::2
  # ─────────────────────────────────────────────
  config.vm.define "node0" do |node|
    node.vm.hostname = "node0"

    # Internal network to Node1 (net01)
    node.vm.network "private_network",
      virtualbox__intnet: "net01",
      auto_config: false

    node.vm.provider "virtualbox" do |vb|
      vb.name   = "Node0"
      vb.memory = 512
      vb.cpus   = 1
      vb.customize ["modifyvm", :id, "--paravirtprovider", "none"]

      # Adapter 2 — net01
      vb.customize ["modifyvm", :id, "--nic2", "intnet"]
      vb.customize ["modifyvm", :id, "--intnet2", "net01"]
      vb.customize ["modifyvm", :id, "--nictype2", "82540EM"]
      vb.customize ["modifyvm", :id, "--nicpromisc2", "deny"]
      vb.customize ["modifyvm", :id, "--macaddress2", "0800277E2227"]
    end

    node.vm.provision "shell", inline: <<~SHELL
      set -e

      # Install ifupdown and disable netplan so /etc/network/interfaces is used
      apt-get update -qq
      apt-get install -y ifupdown make build-essential python3-dev
    #   systemctl stop systemd-networkd networking 2>/dev/null || true
#       mkdir -p /etc/netplan
#       find /etc/netplan -name '*.yaml' -delete
#       cat > /etc/netplan/00-disable.yaml <<'NETPLAN'
# network:
#   version: 2
#   renderer: networkd
# NETPLAN

      # /etc/network/interfaces — enp0s8 (net01)
      cat >> /etc/network/interfaces <<'EOF'

auto enp0s8
iface enp0s8 inet6 static
    address fd00:01::1
    netmask 64
    gateway fd00:01::2
    post-up sysctl -w net.ipv6.conf.all.forwarding=1
    post-up ip -6 route add fd00:12::/64 via fd00:01::2
    post-up ip -6 route add fd00::/64   via fd00:01::2
    post-up ip -6 route add fd00:22::/64 via fd00:01::2
    post-up ip -6 route add fd00:33::/64 via fd00:01::2
EOF

      ifup enp0s8 || true
    SHELL
  end

  # ─────────────────────────────────────────────
  # NODE 1 — DTN Node
  # Interfaces:
  #   Adapter 1 (NAT)    — enp0s3 (keep active, required)
  #   Adapter 2 (net12)  — enp0s8  fd00:12::1/64
  #   Adapter 3 (net01)  — enp0s9  fd00:01::2/64
  #   TUN: fd00::1/64    (tun0, managed by systemd service)
  #   lwIP: fd00::2
  # ─────────────────────────────────────────────
  config.vm.define "node1" do |node|
    node.vm.hostname = "node1"

    node.vm.network "private_network",
      virtualbox__intnet: "net12",
      auto_config: false

    node.vm.network "private_network",
      virtualbox__intnet: "net01",
      auto_config: false

    node.vm.provider "virtualbox" do |vb|
      vb.name   = "Node1"
      vb.memory = 512
      vb.cpus   = 1

      # Adapter 2 — net12
      vb.customize ["modifyvm", :id, "--nic2", "intnet"]
      vb.customize ["modifyvm", :id, "--intnet2", "net12"]
      vb.customize ["modifyvm", :id, "--nictype2", "82540EM"]
      vb.customize ["modifyvm", :id, "--nicpromisc2", "deny"]
      vb.customize ["modifyvm", :id, "--macaddress2", "0800278708BB"]

      # Adapter 3 — net01
      vb.customize ["modifyvm", :id, "--nic3", "intnet"]
      vb.customize ["modifyvm", :id, "--intnet3", "net01"]
      vb.customize ["modifyvm", :id, "--nictype3", "82540EM"]
      vb.customize ["modifyvm", :id, "--nicpromisc3", "deny"]
      vb.customize ["modifyvm", :id, "--macaddress3", "0800271E4112"]
    end

    node.vm.provision "shell", inline: <<~SHELL
      set -e

      # --- ADDED: PERSISTENT ENV VARIABLES FOR C CODE ---
# For interactive login shells (bash scripts, manual use)
cat > /etc/profile.d/dtn_env.sh <<EOF
export HOST_TUN_IPV6_ADDR="fd00::1"
export HOST_LWIP_IPV6_ADDR="fd00::2"
export HOST_enp0s9_IPV6_ADDR="fd00:01::2"
export HOST_enp0s8_IPV6_ADDR="fd00:12::1"
export CURR_NODE_ADDR="fd00:12::1"
export NODE_ID="1"
export CONTACT_PLAN_PATH_DEFAULT="#{CONTACT_PLAN_PATH}"
EOF

# For sudo / systemd / non-login processes — no 'export', no quotes around values
cat >> /etc/environment <<EOF
HOST_TUN_IPV6_ADDR=fd00::1
HOST_LWIP_IPV6_ADDR=fd00::2
HOST_enp0s9_IPV6_ADDR=fd00:01::2
HOST_enp0s8_IPV6_ADDR=fd00:12::1
CURR_NODE_ADDR=fd00:12::1
NODE_ID=1
CONTACT_PLAN_PATH_DEFAULT=#{CONTACT_PLAN_PATH}
EOF

      # Install ifupdown and disable netplan so /etc/network/interfaces is used
      apt-get update -qq
      apt-get install -y ifupdown make build-essential python3-dev

      # /etc/network/interfaces
      cat >> /etc/network/interfaces <<'EOF'

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
EOF

      ifup enp0s8 || true
      ifup enp0s9 || true
      # sysctl -w net.ipv6.conf.all.forwarding=1

      # /etc/systemd/system/tun0.service
      cat > /etc/systemd/system/tun0.service <<'EOF'
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
EOF

      # /usr/local/sbin/setup-tun0.sh (Node 1)
      cat > /usr/local/sbin/setup-tun0.sh <<'EOF'
#!/bin/bash

set -e
ip tuntap add dev tun0 mode tun || true
ip addr add 10.0.0.1/24 dev tun0 || true
ip -6 addr add fd00::1/64 dev tun0 || true
ip link set tun0 up

ip6tables -t mangle -F PREROUTING
ip6tables -t mangle -F OUTPUT
ip6tables -t filter -F INPUT
ip -6 rule del prio 10000 fwmark 1 table 100 2>/dev/null || true
ip -6 rule del prio 10000 2>/dev/null || true

ip6tables -t mangle -A PREROUTING -d fd00:01::2 -j TEE --gateway fd00::2
ip6tables -t mangle -A PREROUTING -d fd00:12::1 -j TEE --gateway fd00::2
ip6tables -t filter -A INPUT -d fd00:01::2 -p ipv6-icmp --icmpv6-type neighbour-solicitation -j ACCEPT
ip6tables -t filter -A INPUT -d fd00:01::2 -p ipv6-icmp --icmpv6-type neighbour-advertisement -j ACCEPT
ip6tables -t filter -A INPUT -d fd00:12::1 -p ipv6-icmp --icmpv6-type neighbour-solicitation -j ACCEPT
ip6tables -t filter -A INPUT -d fd00:12::1 -p ipv6-icmp --icmpv6-type neighbour-advertisement -j ACCEPT
ip6tables -t filter -A INPUT -d fd00:01::2 -j DROP
ip6tables -t filter -A INPUT -d fd00:12::1 -j DROP
ip6tables -t mangle -A PREROUTING -i enp0s8 -m addrtype ! --dst-type LOCAL -j MARK --set-mark 1
ip6tables -t mangle -A PREROUTING -i enp0s9 -m addrtype ! --dst-type LOCAL -j MARK --set-mark 1
ip6tables -t mangle -A OUTPUT -j MARK --set-mark 1

if ! ip -6 rule list | grep -q "fwmark 1.*lookup 100"; then
  ip -6 rule add fwmark 1 table 100 priority 10000
fi
ip -6 route replace default via fd00::2 dev tun0 table 100
EOF

      chmod +x /usr/local/sbin/setup-tun0.sh
      systemctl daemon-reload
      systemctl enable tun0.service
      systemctl start tun0.service || bash -x /usr/local/sbin/setup-tun0.sh
    SHELL
  end
  # ─────────────────────────────────────────────
  # NODE 2 — DTN Node
  # Interfaces:
  #   Adapter 1 (NAT)    — enp0s3 (keep active, required)
  #   Adapter 2 (net12)  — enp0s8  fd00:12::2/64
  #   Adapter 3 (net23)  — enp0s9  fd00:23::2/64
  #   TUN: fd00:22::1/64 (tun0, managed by systemd service)
  #   lwIP: fd00:22::2
  # ─────────────────────────────────────────────
  config.vm.define "node2" do |node|
    node.vm.hostname = "node2"

    node.vm.network "private_network",
      virtualbox__intnet: "net12",
      auto_config: false

    node.vm.network "private_network",
      virtualbox__intnet: "net23",
      auto_config: false

    node.vm.provider "virtualbox" do |vb|
      vb.name   = "Node2"
      vb.memory = 512
      vb.cpus   = 1

      # Adapter 2 — net12
      vb.customize ["modifyvm", :id, "--nic2", "intnet"]
      vb.customize ["modifyvm", :id, "--intnet2", "net12"]
      vb.customize ["modifyvm", :id, "--nictype2", "82540EM"]
      vb.customize ["modifyvm", :id, "--nicpromisc2", "deny"]
      vb.customize ["modifyvm", :id, "--macaddress2", "08002717F434"]

      # Adapter 3 — net23
      vb.customize ["modifyvm", :id, "--nic3", "intnet"]
      vb.customize ["modifyvm", :id, "--intnet3", "net23"]
      vb.customize ["modifyvm", :id, "--nictype3", "82540EM"]
      vb.customize ["modifyvm", :id, "--nicpromisc3", "deny"]
      vb.customize ["modifyvm", :id, "--macaddress3", "0800271182F7"]
    end

    node.vm.provision "shell", inline: <<~SHELL
      set -e

      # --- ADDED: PERSISTENT ENV VARIABLES FOR C CODE ---
      cat > /etc/profile.d/dtn_env.sh <<'EOF'
export HOST_TUN_IPV6_ADDR="fd00:22::1"
export HOST_LWIP_IPV6_ADDR="fd00:22::2"
export HOST_enp0s9_IPV6_ADDR="fd00:23::2"
export HOST_enp0s8_IPV6_ADDR="fd00:12::2"
export CURR_NODE_ADDR="fd00:12::2"
export NODE_ID="2"
export CONTACT_PLAN_PATH_DEFAULT="#{CONTACT_PLAN_PATH}"
EOF

      # Install ifupdown and disable netplan so /etc/network/interfaces is used
      apt-get update -qq
      apt-get install -y ifupdown make build-essential python3-dev

      # /etc/network/interfaces
      cat >> /etc/network/interfaces <<'EOF'

auto enp0s8
iface enp0s8 inet6 static
  address fd00:12::2
  netmask 64
  post-up ip -6 route add fd00::/64    via fd00:12::1
  post-up ip -6 route add fd00:01::/64 via fd00:12::1

auto enp0s9
iface enp0s9 inet6 static
  address fd00:23::2
  netmask 64
  post-up ip -6 route add fd00:33::/64 via fd00:23::3
  post-up sysctl -w net.ipv6.conf.all.forwarding=1
EOF

      ifup enp0s8 || true
      ifup enp0s9 || true
      sysctl -w net.ipv6.conf.all.forwarding=1

      # /etc/systemd/system/tun0.service
      cat > /etc/systemd/system/tun0.service <<'EOF'
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
EOF

      # /usr/local/sbin/setup-tun0.sh (Node 2)
      cat > /usr/local/sbin/setup-tun0.sh <<'EOF'
#!/bin/bash
set -e
ip tuntap add dev tun0 mode tun || true
ip addr add 10.0.2.1/24 dev tun0 || true
ip -6 addr add fd00:22::1/64 dev tun0 || true
ip link set tun0 up

# ip6tables -t mangle -F PREROUTING
# ip6tables -t mangle -F OUTPUT
# ip6tables -t filter -F INPUT
# ip -6 rule del prio 10000 fwmark 1 table 100 2>/dev/null || true
# ip -6 rule del prio 10000 2>/dev/null || true

# ip6tables -t mangle -A PREROUTING -d fd00:23::2 -j TEE --gateway fd00::2
# ip6tables -t mangle -A PREROUTING -d fd00:12::2 -j TEE --gateway fd00::2
# ip6tables -t filter -A INPUT -d fd00:23::2 -j DROP
# ip6tables -t filter -A INPUT -d fd00:12::2 -j DROP

# ip6tables -t mangle -A PREROUTING -i enp0s8 -m addrtype ! --dst-type LOCAL -j MARK --set-mark 1
# ip6tables -t mangle -A PREROUTING -i enp0s9 -m addrtype ! --dst-type LOCAL -j MARK --set-mark 1
# ip6tables -t mangle -A OUTPUT -j MARK --set-mark 1

# if ! ip -6 rule list | grep -q "fwmark 1.*lookup 100"; then
#   ip -6 rule add fwmark 1 table 100 priority 10000
# fi
# ip -6 route replace default via fd00:22::2 dev tun0 table 100
EOF

      chmod +x /usr/local/sbin/setup-tun0.sh
      systemctl daemon-reload
      systemctl enable tun0.service
      systemctl start  tun0.service || bash -x /usr/local/sbin/setup-tun0.sh
    SHELL
  end

  # ─────────────────────────────────────────────
  # NODE 3 — DTN Destination
  # Interfaces:
  #   Adapter 1 (NAT)    — enp0s3 (keep active, required)
  #   Adapter 2 (net23)  — enp0s8  fd00:23::3/64, gw fd00:23::2
  #   TUN: fd00:33::1/64 (tun0, managed by systemd service)
  #   lwIP: fd00:33::2
  # ─────────────────────────────────────────────
  config.vm.define "node3" do |node|
    node.vm.hostname = "node3"

    node.vm.network "private_network",
      virtualbox__intnet: "net23",
      auto_config: false

    node.vm.provider "virtualbox" do |vb|
      vb.name   = "Node3"
      vb.memory = 512
      vb.cpus   = 1

      # Adapter 2 — net23
      vb.customize ["modifyvm", :id, "--nic2", "intnet"]
      vb.customize ["modifyvm", :id, "--intnet2", "net23"]
      vb.customize ["modifyvm", :id, "--nictype2", "82540EM"]
      vb.customize ["modifyvm", :id, "--nicpromisc2", "deny"]
      vb.customize ["modifyvm", :id, "--macaddress2", "080027CFBD9A"]
    end

    node.vm.provision "shell", inline: <<~SHELL
      set -e

      # --- ADDED: PERSISTENT ENV VARIABLES FOR C CODE ---
      cat > /etc/profile.d/dtn_env.sh <<'EOF'
export HOST_TUN_IPV6_ADDR="fd00:33::1"
export HOST_LWIP_IPV6_ADDR="fd00:33::2"
export HOST_enp0s9_IPV6_ADDR="fd00:23::3"
export CURR_NODE_ADDR="fd00:23::3"
export NODE_ID="3"
export CONTACT_PLAN_PATH_DEFAULT="#{CONTACT_PLAN_PATH}"
EOF

      # Install ifupdown and disable netplan so /etc/network/interfaces is used
      apt-get update -qq
      apt-get install -y ifupdown make build-essential python3-dev

      # /etc/network/interfaces
      cat >> /etc/network/interfaces <<'EOF'

auto enp0s8
iface enp0s8 inet6 static
  address fd00:23::3
  netmask 64
  gateway fd00:23::2
  post-up ip -6 route add fd00::/64    via fd00:23::2
  post-up ip -6 route add fd00:01::/64 via fd00:23::2
  post-up ip -6 route add fd00:12::/64 via fd00:23::2
  post-up ip -6 route add fd00:22::/64 via fd00:23::2
  post-up sysctl -w net.ipv6.conf.all.forwarding=1
EOF

      ifup enp0s8 || true
      sysctl -w net.ipv6.conf.all.forwarding=1

      # /etc/systemd/system/tun0.service
      cat > /etc/systemd/system/tun0.service <<'EOF'
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
EOF

      # /usr/local/sbin/setup-tun0.sh (Node 3)
      cat > /usr/local/sbin/setup-tun0.sh <<'EOF'
#!/bin/bash
set -e
ip tuntap add dev tun0 mode tun || true
ip addr add 10.0.0.2/24 dev tun0 || true
ip -6 addr add fd00:33::1/64 dev tun0 || true
ip link set tun0 up

# ip6tables -t mangle -F PREROUTING
# ip6tables -t mangle -F OUTPUT
# ip6tables -t filter -F INPUT
# ip -6 rule del prio 10000 fwmark 1 table 100 2>/dev/null || true
# ip -6 rule del prio 10000 2>/dev/null || true

# ip6tables -t mangle -A PREROUTING -d fd00:23::3 -j TEE --gateway fd00::2
# ip6tables -t filter -A INPUT -d fd00:23::3 -j DROP

# ip6tables -t mangle -A PREROUTING -i enp0s8 -m addrtype ! --dst-type LOCAL -j MARK --set-mark 1
# ip6tables -t mangle -A OUTPUT -j MARK --set-mark 1

# if ! ip -6 rule list | grep -q "fwmark 1.*lookup 100"; then
#   ip -6 rule add fwmark 1 table 100 priority 10000
# fi
# ip -6 route replace default via fd00:33::2 dev tun0 table 100
EOF

      chmod +x /usr/local/sbin/setup-tun0.sh
      systemctl daemon-reload
      systemctl enable tun0.service
      systemctl start tun0.service || bash -x /usr/local/sbin/setup-tun0.sh
    SHELL
  end

end