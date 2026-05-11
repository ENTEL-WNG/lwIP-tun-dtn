#!/bin/bash

python3 init_node.py node2.toml

echo "--- Setup Complete. Starting LwIP binary ---"
sleep 2
exec stdbuf -oL -eL ./lwip_tun 2>&1