#!/bin/sh

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Ping 1: after 5s
sh "$SCRIPT_DIR/../ping.sh" 5 node1 node3 fd00:2:3::3