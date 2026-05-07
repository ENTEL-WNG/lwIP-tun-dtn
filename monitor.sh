#!/bin/bash
NODE_NAME=${1:-node0} # Use the node name (node0, node1, etc.)
NODE_PORT=${2:-2222} 

# Dynamically find the private key for this specific node
KEY_PATH=".vagrant/machines/${NODE_NAME}/virtualbox/private_key"

echo "Connecting to $NODE_NAME on port $NODE_PORT..."

ssh -i "$KEY_PATH" -p "$NODE_PORT" \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    vagrant@127.0.0.1 \
    "sudo tcpdump -i any -U -s 0 -w - 'not port 22'" | \
    wireshark -k -i -