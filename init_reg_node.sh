#!/bin/bash
set -e
echo "--- DTN $NODE: Initializing Regular Node ---"

sysctl -w net.ipv6.conf.all.accept_dad=0 || true
# sysctl -w net.ipv6.conf.all.forwarding=0 || true
sysctl -w net.ipv6.conf.all.forwarding=1 || true

./init_routes.sh

echo "--- Node $NODE Setup Complete. Keeping container alive ---"

# Keep the container running so you can exec into it and run ping etc.
exec sleep infinity

