#!/bin/bash

tcpdump -i any -n ip6 -U -w - | \
    tee /captures/node_${NODE}_$(date +%Y-%m-%d_%H-%M-%S).pcap | \
    tcpdump -r - -n



# docker exec -it dtn_node_1 sh ./capture.sh
# docker exec -it dtn_node_2 sh ./capture.sh
# docker exec -it dtn_node_3 sh ./capture.sh