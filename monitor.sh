#!/bin/bash

# First start with docker compose up || docekr compose up -d

docker exec traffic_monitor tcpdump -i any -U -w - "(ip6)" | \
  tee captures/all_$(date +%Y%m%d_%H%M%S).pcap | \
  wireshark -k -i -

docker exec traffic_node_3 tcpdump -i any -U -w - | \
  tee captures/node_3_$(date +%Y%m%d_%H%M%S).pcap | \
  wireshark -k -i -


docker exec traffic_node_3 which dumpcap

docker exec traffic_node_3 \
  dumpcap -i any -P -w - | \
  tee captures/node_3$(date +%Y%m%d_%H%M%S).pcapng | \
  wireshark -k -i -

docker exec traffic_node_3 \
  dumpcap -i any -w - | \
  tee captures/node_3$(date +%Y%m%d_%H%M%S).pcapng | \
  wireshark -k -i -