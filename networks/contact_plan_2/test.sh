#!/bin/sh

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Ping 1: after 5s
sh "$SCRIPT_DIR/../ping.sh" 5 node1 node4 fd00:4:5::4

# Ping 2: after 20s
sh "$SCRIPT_DIR/../ping.sh" 20 node1 node4 fd00:4:5::4
