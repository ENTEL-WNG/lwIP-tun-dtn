#!/bin/sh
set -e
CAPTURES=/repo/captures/contact_plan_2/$TEST_CASE_NUMBER
mkdir -p "$CAPTURES"
for BR in $(ip link show type bridge | awk -F': ' '/^[0-9]+:/{print $2}'); do
    tcpdump -i "$BR" ip6 -nn -U -w "$CAPTURES/capture.pcap" &
    tcpdump -i "$BR" ip6 -nn -e -l -tttt >> "$CAPTURES/capture.txt" 2>&1 &
done
wait
