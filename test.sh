#!/bin/bash

# Define your list of nodes here
TARGETS=(
    "fd00:01::1" 
    "fd00:01::2"
    "fd00::1"
    "fd00::2"
    "fd00:12::1"
    "fd00:12::2"
    "fd00:23::2"
    "fd00:23::3"
    "fd00:22::2"
    "fd00:33::3"
)

echo "Starting Network Diagnostics..."
echo "-----------------------------"

for IP in "${TARGETS[@]}"; do
    echo "Testing: $IP"
    
    # -c 1: send one packet
    # -W 2: wait 2 seconds for a response
    # -v  : verbose output (useful for seeing ICMP errors)
    if ping6 -c 1 -W 2 -v "$IP" > /dev/null 2>&1; then
        echo "✅ SUCCESS: $IP is reachable."
    else
        echo "❌ ERROR: $IP is unreachable."
        # Optional: check the neighbor table status for this IP
        ip -6 neighbor show "$IP" | sed 's/^/   -> /'
    fi
    echo "-----------------------------"
done