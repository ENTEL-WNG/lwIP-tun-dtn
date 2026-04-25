#!/bin/bash

# First start with docker compose up || docekr compose up -d

docker exec traffic_monitor tcpdump -i any -U -w - "(ip6)" | \
  tee captures/all_$(date +%Y-%m-%d_%H-%M-%S).pcap | \
  wireshark -k -i -

# docker exec traffic_monitor tcpdump -i any -n ip6 -U -w | \
#   tee captures/all_$(date +%Y-%m-%d_%H-%M-%S).pcap | \
#   wireshark -k -i -