#!/bin/sh

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# UDP: after 5s, send multiple messages
sh "$SCRIPT_DIR/../udp.sh" 5 node1 node3 fd00:2:3::3 5005 5 "msg-one" "msg-two" "msg-three"