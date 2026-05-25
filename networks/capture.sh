#!/bin/sh
# Capture all IPv6 traffic in the host network namespace.
#
# Running with network_mode: host places this container in the host's network
# namespace, where the Docker bridge interfaces (br-xxxx) are visible.
# Capturing on a bridge interface sees ALL frames — including unicast between
# containers — because the bridge kernel code processes every frame there,
# before the FDB decides which port to forward to.
#
# Two files are written per run:
#   capture.pcap  — binary pcap, open with Wireshark
#   capture.txt   — human-readable text with timestamps and interface names
#
# Environment variables (set by docker-compose):
#   PLAN_NAME        — contact plan directory name, e.g. contact_plan_2
#   TEST_CASE_NUMBER — run index, e.g. 0

set -e

CAPTURES="/repo/networks/${PLAN_NAME}/captures/${TEST_CASE_NUMBER}"
mkdir -p "$CAPTURES"

echo "Capturing to $CAPTURES/capture.{pcap,txt}"

rm -f "${CAPTURES}/capture.pcap" "${CAPTURES}/capture.txt"

# Find all Docker bridge interfaces in the host namespace.
# We pick the first one to write the pcap/txt (all bridges carry the same
# inter-node traffic since each link is a separate bridge — one bridge per
# node pair). To see ALL links in one file we use -i any which captures
# across every interface in the host namespace.

# -i any    : capture on all host interfaces (includes all br-xxxx bridges)
# ip6       : IPv6 traffic only
# -nn       : no DNS / port name resolution
# -U        : write each packet immediately (unbuffered)
# -e        : print interface name and link-layer header per packet (txt only)
# -tttt     : full absolute timestamp with date (txt only)

tcpdump -i any ip6 -nn -U \
    --direction=inout \
    -w "${CAPTURES}/capture.pcap" &

tcpdump -i any ip6 -nn -e -l -tttt \
    --direction=inout \
    > "${CAPTURES}/capture.txt" 2>&1 &

wait
