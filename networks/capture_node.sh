#!/bin/sh
# Per-node traffic capture script.
# Called by init_node.py before os.execv into lwip_tun.
# The tcpdump processes outlive the exec and keep capturing under lwip_tun.
#
# Usage: sh capture_node.sh <node_id> <plan_name>
#
# Environment variables:
#   TEST_CASE_NUMBER — run index set by docker-compose (default: 0)
#
# Two files are written:
#   /repo/networks/<plan>/captures/<run>/node<id>.pcap  — binary pcap
#   /repo/networks/<plan>/captures/<run>/node<id>.txt   — human-readable text

NODE_ID="$1"
PLAN_NAME="$2"
TEST_CASE_NUMBER="${TEST_CASE_NUMBER:-0}"

CAPTURES="/repo/networks/${PLAN_NAME}/captures/${TEST_CASE_NUMBER}"
BASE="${CAPTURES}/node${NODE_ID}"

mkdir -p "$CAPTURES"
rm -f "${BASE}.pcap" "${BASE}.txt"

echo "Capturing node${NODE_ID} -> ${BASE}.{pcap,txt}"

# -i any  : all interfaces (physical links + tun0)
# ip6     : IPv6 only
# -nn     : no DNS/port resolution
# -U      : unbuffered writes to pcap
# -e      : interface name + link-layer header (txt only)
# -tttt   : full absolute timestamp (txt only)

tcpdump -i any ip6 -nn -U \
    -w "${BASE}.pcap" &

tcpdump -i any ip6 -nn -e -l -tttt \
    > "${BASE}.txt" 2>&1 &

# xtables-monitor --trace shows each packet's traversal through netfilter
# rules. Requires TRACE rules in ip6tables raw table (set up by init_node.py).
xtables-monitor --trace \
    > "${BASE}_trace.txt" 2>&1 &


