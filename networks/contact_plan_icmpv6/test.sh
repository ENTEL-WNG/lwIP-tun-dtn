#!/bin/sh

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# UDP: after 5s, send multiple messages
sh "$SCRIPT_DIR/../udp.sh" 5 node1 node4 fd00:3:4::4 5005 2 "msg-one"